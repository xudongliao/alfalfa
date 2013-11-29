#include <iostream>

#include "vp8_parser.hh"
#include "uncompressed_chunk.hh"
#include "frame.hh"

using namespace std;

VP8Parser::VP8Parser( uint16_t s_width, uint16_t s_height )
  : width_( s_width ), height_( s_height )
{
}

void VP8Parser::parse_frame( const Chunk & frame )
{
  /* parse uncompressed data chunk */
  UncompressedChunk uncompressed_chunk( frame, width_, height_ );

  /* only parse key frames for now */
  if ( !uncompressed_chunk.key_frame() ) {
    return;
  }

  KeyFrame( uncompressed_chunk, width_, height_ );
}