#include "loopfilter.hh"
#include "frame_header.hh"
#include "macroblock_header.hh"
#include "raster.hh"
#include "loopfilter_filters.hh"

static inline uint8_t clamp63( const int input )
{
  if ( input > 63 ) return 63;
  if ( input < 0 ) return 0;
  return input;
}

FilterParameters::FilterParameters( const KeyFrameHeader & header )
  : type( header.filter_type ? LoopFilterType::Simple : LoopFilterType::Normal ),
    filter_level( header.loop_filter_level ),
    sharpness_level( header.sharpness_level )
{}

FilterParameters::FilterParameters( const uint8_t segment_id,
				    const KeyFrameHeader & header,
				    const Optional< UpdateSegmentation > & update_segmentation )
  : FilterParameters( header )
{
  if ( update_segmentation.initialized()
       and update_segmentation.get().segment_feature_data.initialized() ) {
    const auto & feature_data = update_segmentation.get().segment_feature_data.get();
    const auto & update = feature_data.loop_filter_update.at( segment_id );

    if ( update.initialized() ) {
      if ( feature_data.segment_feature_mode ) { /* absolute value */
	if ( update.get() < 0 or update.get() > 63 ) {
	  throw Invalid( "absolute loop-filter update with out-of-bounds value" );
	}
	filter_level = update.get();
      } else { /* delta */
	filter_level += update.get();
      }
    }
  }
}

static int8_t mode_adjustment( const SafeArray< int8_t, 4 > & mode_adjustments,
			       const reference_frame macroblock_reference_frame,
			       const intra_mbmode macroblock_y_mode )
{
  if ( macroblock_reference_frame == CURRENT_FRAME ) {
    return ( macroblock_y_mode == B_PRED ) ? mode_adjustments.at( 0 ) : 0;
    /*  } else if ( macroblock_y_mode == ZEROMV ) {
	return 1;
	} else if ( macroblock_y_mode == SPLITMV ) {
	return 3; */
  } else {
    return mode_adjustments.at( 2 );
  }
}

void FilterParameters::adjust( const SafeArray< int8_t, num_reference_frames > & ref_adjustments,
			       const SafeArray< int8_t, 4 > & mode_adjustments,
			       const reference_frame macroblock_reference_frame,
			       const intra_mbmode macroblock_y_mode )
{
  filter_level += ref_adjustments.at( macroblock_reference_frame )
    + mode_adjustment( mode_adjustments, macroblock_reference_frame, macroblock_y_mode );
}

SimpleLoopFilter::SimpleLoopFilter( const FilterParameters & params )
  : filter_level_( clamp63( params.filter_level ) ),
    interior_limit_( filter_level_ ),
    macroblock_edge_limit_(),
    subblock_edge_limit_()
{
  if ( params.sharpness_level ) {
    interior_limit_ >>= params.sharpness_level > 4 ? 2 : 1;

    if ( interior_limit_ > 9 - params.sharpness_level ) {
      interior_limit_ = 9 - params.sharpness_level;
    }
  }

  if ( interior_limit_ < 1 ) {
    interior_limit_ = 1;
  }

  macroblock_edge_limit_ = ( ( filter_level_ + 2 ) * 2 ) + interior_limit_;
  subblock_edge_limit_ = ( filter_level_ * 2 ) + interior_limit_;
}

NormalLoopFilter::NormalLoopFilter( const bool key_frame,
				    const FilterParameters & params )
  : simple_( params ),
    high_edge_variance_threshold_( simple_.filter_level() >= 15 )
{
  assert( params.type == LoopFilterType::Normal );

  if ( simple_.filter_level() >= 40 ) {
    high_edge_variance_threshold_++;
  }

  if ( simple_.filter_level() >= 20 and (not key_frame) ) {
    high_edge_variance_threshold_++;
  }
}

void KeyFrameMacroblockHeader::loopfilter( const KeyFrameHeader::DerivedQuantities & derived )
{
  const bool skip_subblock_edges = ( Y2_.prediction_mode() != B_PRED ) and ( not has_nonzero_ );

  /* which filter are we using? */
  FilterParameters filter_parameters( segment_id_.initialized()
				      ? derived.segment_loop_filters.at( segment_id_.get() )
				      : derived.loop_filter );

  filter_parameters.adjust( derived.loopfilter_ref_adjustments,
			    derived.loopfilter_mode_adjustments,
			    CURRENT_FRAME,
			    Y2_.prediction_mode() );

  /* is filter disabled? */
  if ( filter_parameters.filter_level <= 0 ) {
    return;
  }

  switch ( filter_parameters.type ) {
  case LoopFilterType::Normal:
    {
      NormalLoopFilter filter( true, filter_parameters );
      filter.filter( *raster_.get(), skip_subblock_edges );
    }
    break;
  case LoopFilterType::Simple:
    {
      SimpleLoopFilter filter( filter_parameters );
      filter.filter( *raster_.get(), skip_subblock_edges );
    }
    break;
  default:
    assert( false );
  }
}

void SimpleLoopFilter::filter( Raster::Macroblock & , const bool )
{
  assert( false );
}

void NormalLoopFilter::filter( Raster::Macroblock & raster, const bool skip_subblock_edges )
{
  /* 1: filter the left inter-macroblock edge */
  if ( raster.Y.context().left.initialized() ) {
    filter_mb_vertical( raster.Y );
    filter_mb_vertical( raster.U );
    filter_mb_vertical( raster.V );
  }

  /* 2: filter the vertical subblock edges */
  if ( not skip_subblock_edges ) {
    filter_sb_vertical( raster.Y );
    filter_sb_vertical( raster.U );
    filter_sb_vertical( raster.V );
  }

  /* 3: filter the top inter-macroblock edge */
  if ( raster.Y.context().above.initialized() ) {
    filter_mb_horizontal( raster.Y );
    filter_mb_horizontal( raster.U );
    filter_mb_horizontal( raster.V );
  }

  /* 4: filter the horizontal subblock edges */
  if ( not skip_subblock_edges ) {
    filter_sb_horizontal( raster.Y );
    filter_sb_horizontal( raster.U );
    filter_sb_horizontal( raster.V );
  }
}

template <class BlockType>
void NormalLoopFilter::filter_mb_vertical( BlockType & block )
{
  const uint8_t size = BlockType::dimension;

  for ( unsigned int row = 0; row < size; row++ ) {
    uint8_t *central = &block.at( 0, row );

    const int8_t mask = vp8_filter_mask( simple_.interior_limit(),
					 simple_.macroblock_edge_limit(),
					 *(central - 4),
					 *(central - 3),
					 *(central - 2),
					 *(central - 1),
					 *(central),
					 *(central + 1),
					 *(central + 2),
					 *(central + 3) );

    const int8_t hev = vp8_hevmask( high_edge_variance_threshold_,
				    *(central - 2),
				    *(central - 1),
				    *(central),
				    *(central + 1) );

    vp8_mbfilter( mask, hev,
		  *(central - 3), *(central - 2), *(central - 1),
		  *(central), *(central + 1), *(central + 2) );
  }
}

template <class BlockType>
void NormalLoopFilter::filter_mb_horizontal( BlockType & block )
{
  const uint8_t size = BlockType::dimension;

  const unsigned int stride = block.stride();

  for ( unsigned int column = 0; column < size; column++ ) {
    uint8_t *central = &block.at( column, 0 );

    const int8_t mask = vp8_filter_mask( simple_.interior_limit(),
					 simple_.macroblock_edge_limit(),
					 *(central - 4 * stride ),
					 *(central - 3 * stride ),
					 *(central - 2 * stride ),
					 *(central - stride ),
					 *(central),
					 *(central + stride ),
					 *(central + 2 * stride ),
					 *(central + 3 * stride ) );

    const int8_t hev = vp8_hevmask( high_edge_variance_threshold_,
				    *(central - 2 * stride ),
				    *(central - stride ),
				    *(central),
				    *(central + stride ) );

    vp8_mbfilter( mask, hev,
		  *(central - 3 * stride ),
		  *(central - 2 * stride ),
		  *(central - stride ),
		  *(central),
		  *(central + stride ),
		  *(central + 2 * stride ) );
  }
}

template <class BlockType>
void NormalLoopFilter::filter_sb_vertical( BlockType & block )
{
  const uint8_t size = BlockType::dimension;

  for ( unsigned int center_column = 4; center_column < size; center_column += 4 ) {
    for ( unsigned int row = 0; row < size; row++ ) {
      uint8_t *central = &block.at( center_column, row );

      const int8_t mask = vp8_filter_mask( simple_.interior_limit(),
					   simple_.subblock_edge_limit(),
					   *(central - 4),
					   *(central - 3),
					   *(central - 2),
					   *(central - 1),
					   *(central),
					   *(central + 1),
					   *(central + 2),
					   *(central + 3) );

      const int8_t hev = vp8_hevmask( high_edge_variance_threshold_,
				      *(central - 2),
				      *(central - 1),
				      *(central),
				      *(central + 1) );

      vp8_filter( mask, hev,
		  *(central - 2), *(central - 1),
		  *(central), *(central + 1) );
    }
  }
}

template <class BlockType>
void NormalLoopFilter::filter_sb_horizontal( BlockType & block )
{
  const uint8_t size = BlockType::dimension;

  const unsigned int stride = block.stride();

  for ( unsigned int center_row = 4; center_row < size; center_row += 4 ) {
    for ( unsigned int column = 0; column < size; column++ ) {
      uint8_t *central = &block.at( column, center_row );

      const int8_t mask = vp8_filter_mask( simple_.interior_limit(),
					   simple_.subblock_edge_limit(),
					   *(central - 4 * stride ),
					   *(central - 3 * stride ),
					   *(central - 2 * stride ),
					   *(central - stride ),
					   *(central),
					   *(central + stride ),
					   *(central + 2 * stride ),
					   *(central + 3 * stride ) );

      const int8_t hev = vp8_hevmask( high_edge_variance_threshold_,
				      *(central - 2 * stride ),
				      *(central - stride ),
				      *(central),
				      *(central + stride ) );

      vp8_filter( mask, hev,
		  *(central - 2 * stride ),
		  *(central - stride ),
		  *(central),
		  *(central + stride ) );
    }
  }
}