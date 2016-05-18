// The MIT License
//
// Copyright (c) 2014 Institute for Systems Biology
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

// The MZGFile library is modeled directly off of the work done by Bob Handsaker
// from the Broad Institute and by the SAMtools developers in the BGZF library.

#include <assert.h>
#include <cstdio>
#include <cerrno>
#include <ctime>
#include <iostream>
#include <iomanip>
#include <getopt.h>
#include <sys/stat.h>

#include "MZGFile.h"
#include "zlib.h"

#define log_debug( fmt, args...) \

//   do { fprintf(stderr, "%s(): " fmt, __func__, ##args); } while (0)

#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>
#  include <io.h>
#  define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#  define SET_BINARY_MODE(file)
#endif

//
// GZIP header (from RFC 1952; little endian):
//
// +---+---+---+---+---+---+---+---+---+---+
// |ID1|ID2|CM |FLG|     MTIME     |XFL|OS | (more-->)
// +---+---+---+---+---+---+---+---+---+---+
// (if FLG.FEXTRA set)
// +---+---+---+---+========================================+
// | XLEN  |SI1|SI2|  LEN  | ..LEN bytes of subfield data...| (more-->)
// +---+---+---+---+========================================+
// (if FLG.FNAME set)
// +=========================================+
// |...original file name, zero-terminated...| (more-->)
// +=========================================+
// (if FLG.FCOMMENT set)
// +===================================+
// |...file comment, zero-terminated...| (more-->)
// +===================================+
// (if FLG.FHCRC set)
// +---+---+
// | CRC16 |
// +---+---+
// +=======================+
// |...compressed blocks...| (more-->)
// +=======================+
//   0   1   2   3   4   5   6   7
// +---+---+---+---+---+---+---+---+
// |     CRC32     |     ISIZE     |
// +---+---+---+---+---+---+---+---+
//
// MZGF utilizes the extra fields in the GZIP header as follows:
//
//  SI1 = 'M', SI2 = 'Z', EXT1   = MZGF version
//  SI1 = 'B', SI2 = 'O', EXT1-4 = offset to block index
//

#define GZIP_MAGIC_ID1     0x1f
#define GZIP_MAGIC_ID2     0x8b
#define GZIP_CM_DEFLATED   8
#define GZIP_FTEXT_FLG     0x01
#define GZIP_FHCRC_FLG     0x02
#define GZIP_FEXTRA_FLG    0x04
#define GZIP_FNAME_FLG     0x08
#define GZIP_FCOMMENT_FLG  0x10
#define GZIP_FEXTRA_MAX    0xFFFF

#ifdef _WIN32
#define GZIP_OS            0
#elif __linux
#define GZIP_OS            3
#else
#define GZIP_OS            255
#endif

#define INFLATE_WIN_BITS  -15    // Base log 2 of window size (history buffer)
                                 //    w/o gzip header
#define DEFLATE_WIN_BITS  -15    // Base log 2 of window size (history buffer)
                                 //    w/o gzip header
#define DEFLATE_MEM_LEVEL 8      // How much memory should be allocated by zlib
                                 // when deflating 1=min 9=max

namespace MZGFile {

// Minimal gzip header
static const uint8_t gzheader[] = {
   GZIP_MAGIC_ID1,   // ID1
   GZIP_MAGIC_ID2,   // ID2
   8,                // CM
   GZIP_FEXTRA_FLG,  // FLG
   0, 0, 0, 0,       // MTIME
   0,                // XFL
   GZIP_OS,          // OS
   0, 0              // XLEN
   };

// store mzgf version in a extra field in gzip header
static uint8_t extra_mzgf[] = {
   'M', 'Z',                  // extra field identifier
   1, 0,                      // len of the bytes of subfield data
   MZGF_VERSION               // MZGF format version
};

// store file information in extra field in gzip header
uint8_t extra_eof[] = {
   'B', 'O',                  // extra field identifier
   16, 0,                     // len of the bytes of subfield data
   0, 0, 0, 0, 0, 0, 0, 0,    // size of compressed file
   0, 0, 0, 0, 0, 0, 0, 0     // offset of the 1st block index
};

// store block indexes in a gzip header extra field
static uint8_t extra_bindex[GZIP_FEXTRA_MAX] = {
   'B', 'I',                  // field identifier
   0, 0,                      // len of the bytes of subfield data
   0, 0, 0, 0, 0, 0, 0, 0     // offset of the next gz block containing indexes
};

static inline void packInt16(uint8_t *buffer, uint16_t value)
{
   buffer[0] = value;
   buffer[1] = value >> 8;
}

static inline void packInt32(uint8_t *buffer, uint32_t value)
{
   buffer[0] = value;
   buffer[1] = value >> 8;
   buffer[2] = value >> 16;
   buffer[3] = value >> 24;
}

static inline void packInt64(uint8_t *buffer, uint64_t value)
{
   buffer[0] = value;
   buffer[1] = value >> 8;
   buffer[2] = value >> 16;
   buffer[3] = value >> 24;
   buffer[4] = value >> 32;
   buffer[5] = value >> 40;
   buffer[6] = value >> 48;
   buffer[7] = value >> 56;
}

static inline int unpackInt16(const uint8_t *buffer)
{
   return buffer[0] | buffer[1] << 8;
}

static inline uint32_t unpackInt32(const uint8_t *buffer)
{
   uint32_t r = buffer[0];
   r |= (uint64_t)buffer[1] << 8;
   r |= (uint64_t)buffer[2] << 16;
   r |= (uint64_t)buffer[3] << 24;
   r |= (uint64_t)buffer[4] << 32;
   return r;
}

static inline uint64_t unpackInt64(const uint8_t *buffer)
{
   uint64_t r = buffer[0];
   r |= (uint64_t)buffer[1] << 8;
   r |= (uint64_t)buffer[2] << 16;
   r |= (uint64_t)buffer[3] << 24;
   r |= (uint64_t)buffer[4] << 32;
   r |= (uint64_t)buffer[5] << 40;
   r |= (uint64_t)buffer[6] << 48;
   r |= (uint64_t)buffer[7] << 56;
   return r;
}

MZGFileWriter::MZGFileWriter() {
   m_mtime = time(NULL);
   m_bindex_offset = 0;
}

MZGFileWriter::~MZGFileWriter() {
   (void)deflateEnd( &m_zs );
}

int MZGFileWriter::deflate( FILE *src, FILE *dst ) {
   int ret = 0;

   assert( compressBound(MZGF_BLOCK_SIZE) < MZGF_MAX_BLOCK_SIZE );

   // Initialize deflate. zalloc, zfree, opaque must be set first
   m_zs.zalloc = Z_NULL;
   m_zs.zfree  = Z_NULL;
   m_zs.opaque = Z_NULL;
   if ( 0 != (ret = deflateInit2( &m_zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                                  DEFLATE_WIN_BITS, DEFLATE_MEM_LEVEL,
                                  Z_DEFAULT_STRATEGY )) ) {
      m_error = zError( ret );
      return ret;
   }

   SET_BINARY_MODE( src );
   SET_BINARY_MODE( dst );

   m_fp      = dst;
   m_uoffset = 0;
   m_usize   = 0;
   m_ucrc32  = ::crc32( 0L, NULL, 0L );
   m_zoffset = 0;

   ret = _write_header( extra_mzgf, sizeof(extra_mzgf) );
   if ( ret ) return ret;

   bindex_t bi;
   do {
      m_zs.next_in  = m_ublock;
      m_zs.avail_in = fread( m_ublock, 1, sizeof(m_ublock), src );
      if ( ferror(src) ) {
         m_error = std::strerror( errno ) ? : "unrecognized error reading block";
         return( errno || -1 );
      }
      log_debug( "read block uoffset: %ld avail_in: %d\n", m_uoffset, m_zs.avail_in );

      bi.zoffset = m_zoffset;          // store block index
      bi.uoffset = m_uoffset;
      m_bindex.push_back( bi );
      m_uoffset += m_zs.avail_in;

      if ( 0 != (ret = _flush()) ) {
         return ret;
      }

   } while ( !feof(src) );

   if ( 0 != (ret = _write_trailer()) ) return ret;
   if ( 0 != (ret = _write_bindex()) )  return ret;
   if ( 0 != (ret = _write_eof()) )     return ret;

   m_fp = NULL;
   return ret;
}

//
// Write a GZIP header according to RFC 1952 (see description above).
//
int MZGFileWriter::_write_header( uint8_t *extra, int extralen ) {
   assert( extralen < GZIP_FEXTRA_MAX );

   memcpy( m_zblock, gzheader, sizeof(gzheader) );
   packInt32( &m_zblock[4], m_mtime );                         // set MTIME
   packInt16( &m_zblock[10], extralen );                       // set XLEN
   if ( fwrite( m_zblock, 1, sizeof(gzheader), m_fp ) != sizeof(gzheader)
        || ferror(m_fp) ) {
      m_error = std::strerror(errno) ? : "unrecognized error writing header";
      }
   m_zoffset += sizeof(gzheader);

   if ( extralen ) {
      if ( fwrite( extra, 1, extralen, m_fp ) != extralen || ferror(m_fp) ) {
         m_error = std::strerror(errno) ? : "unrecognized error writing header";
         }
      m_zoffset += extralen;
   }

   return 0;
}

//
// Deflate a block of input and write it to the output
//
int MZGFileWriter::_flush() {
   int ret;

   m_usize += m_zs.avail_in;
   m_ucrc32 = ::crc32( m_ucrc32, m_ublock, m_zs.avail_in );

   // Run deflate() on input until input buffer is empty. In the unlikely case
   // the output buffer becomes full write its contents and keep deflating
   int flush = (m_zs.avail_in < MZGF_BLOCK_SIZE) ? Z_FINISH : Z_FULL_FLUSH;
   int have  = 0;
   do {
      m_zs.next_out  = m_zblock;
      m_zs.avail_out = MZGF_BLOCK_SIZE;

      log_debug( "deflate beg uoffset: %-8ld zoffset: %-8ld "
                 "avail_in: %-5d avail_out: %05d flush: %d\n",
                 m_uoffset, m_zoffset, m_zs.avail_in, m_zs.avail_out, flush );

      switch ( ret = ::deflate( &m_zs, flush ) ) {
         case Z_OK :
         case Z_STREAM_END :
            break;
         case Z_BUF_ERROR :
         case Z_STREAM_ERROR :
            m_error = zError( ret ) ? : "deflate - gzstream/buf error";
            return ret || -1;
            break;
         default :
            m_error = zError( ret ) ? : "deflate - unknown error ";
            return ret || -1;
      }
      have = MZGF_BLOCK_SIZE - m_zs.avail_out;
      if ( fwrite( m_zblock, 1, have, m_fp ) != have || ferror(m_fp) ) {
         m_error = std::strerror(errno) ? : "unrecognized error writing header";
         return errno || -1;
      }
      m_zoffset += have;

      log_debug( "deflate end uoffset: %-8ld zoffset: %-8ld "
                 "avail_in: %-5d avail_out: %-5d  have: %d\n",
                 m_uoffset, m_zoffset, m_zs.avail_in, m_zs.avail_out, have );

   } while ( 0 == m_zs.avail_out );

   return 0;
}

//
// Write "empty" compressed contents
//
int MZGFileWriter::_write_empty() {

   uint8_t empty[2] = { 0x03, 0x00 };

   int have = sizeof(empty);
   if ( fwrite( empty, 1, have, m_fp ) != have || ferror(m_fp) ) {
      m_error = std::strerror(errno) ? : "unrecognized error writing bindex";
      return( errno || - 1 );
   }
   m_zoffset += have;

   // clear
   m_usize  = 0;
   m_ucrc32 = ::crc32( 0L, NULL, 0L );
   return 0;
}

//
// Write the gzip trailer, consisting of a 4 byte CRC32 and 4 byte size of
// data input.
//
int MZGFileWriter::_write_trailer() {
   uint8_t footer[8];
   int     have = sizeof(footer);

   log_debug( "crc32: %x usize %ld\n", m_ucrc32, m_usize );
   packInt32( &footer[0], m_ucrc32 );  // 4 byte CRC32
   packInt32( &footer[4], m_usize );   // 4 byte ISIZE
   if ( fwrite( footer, 1, have, m_fp ) != have || ferror(m_fp) ) {
        m_error = std::strerror(errno) ? : "unrecognized error writing footer";
       return( errno || - 1 );
   }
   m_zoffset += 8;

   return 0;
}

//
// Append the block index to the end of the stream as a set of one or more
// gzip members containing no uncompressed bytes. The indices are stored as a
// extra field in the gzip header.
//
int MZGFileWriter::_write_bindex() {
   int ret;

   log_debug( "write bindex\n");

   off_t next = m_bindex_offset = m_zoffset;
   int offset = 12;           // skip past the subfield id, len, and next offset
   for ( int i = 0; i < m_bindex.size(); i++ ) {
      log_debug( "%d %ld %ld\n", i, m_bindex[i].zoffset, m_bindex[i].uoffset );
      packInt64( &extra_bindex[offset], m_bindex[i].zoffset );
      offset += sizeof(uint64_t);
      packInt64( &extra_bindex[offset], m_bindex[i].uoffset );
      offset += sizeof(uint64_t);

      if ( (offset + sizeof(bindex_t) >= sizeof(extra_bindex)) ||
           (i+1 == m_bindex.size()) ) {
         next = (i+1 == m_bindex.size()) ? 0 : m_zoffset + sizeof(gzheader) + offset + 2 + 8;
         packInt16( &extra_bindex[2], offset-4 );  // account for field id&size
         packInt64( &extra_bindex[4], next );      // store next offset

         log_debug( "bindex header size: %d, next: %ld\n", offset, next );

         // Write gzip member
         if ( 0 != (ret = _write_header( extra_bindex, offset )) ) return ret;
         if ( 0 != (ret = _write_empty()) ) return ret;
         if ( 0 != (ret = _write_trailer()) ) return ret;

         offset = 12;            // start of indexes
      }
   }

   return 0;
}

//
// Writes a empty gzip block indicating the EOF.  This block is of a fixed
// size and may contain pointers to other gzip blocks within the file.
//
int MZGFileWriter::_write_eof() {
   int ret;

   log_debug( "writing eof block at %ld, filesize %ld BI offset %ld\n", 
              ftell(m_fp), m_uoffset, m_bindex_offset );

   packInt64( &extra_eof[4],  m_uoffset );           // store file size
   packInt64( &extra_eof[12], m_bindex_offset );     // store bindex offset

   if ( 0 != (ret = _write_header( extra_eof, sizeof(extra_eof) )) ) return ret;
   if ( 0 != (ret = _write_empty()) ) return ret;
   if ( 0 != (ret = _write_trailer()) ) return ret;

   return 0;
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

MZGFileReader::MZGFileReader() {

   m_version   = -1;
   m_mtime     = 0;
   m_fp        = NULL;
   m_ufilesize = -1;
   m_zfilesize = -1;
   m_isEOF     = false;

   m_zoffset = 0;
   m_uoffset = 0;
   m_blen    = 0;
   m_boffset = 0;

   m_bindex_offset = 0;
}

int MZGFileReader::open( const char *path ) {
   int ret = 0;

   assert( compressBound(MZGF_BLOCK_SIZE) < MZGF_MAX_BLOCK_SIZE );

   // Open the file
   if ( 0 == (m_fp = fopen( path, "rb") ) ) {
      m_error = std::strerror(errno);
      return errno || -1;
   }
   SET_BINARY_MODE(m_fp);
   m_isEOF   = false;
   m_zoffset = 0;
   m_uoffset = 0;

   // Initialize inflate. zalloc, zfree, opaque must be set first
   m_zs.zalloc   = Z_NULL;
   m_zs.zfree    = Z_NULL;
   m_zs.opaque   = Z_NULL;
   m_zs.next_in   = m_zblock;
   m_zs.avail_in = 0;
   m_zs.next_out  = m_ublock;
   m_zs.avail_out = MZGF_BLOCK_SIZE;
   if ( Z_OK != (ret = inflateInit2( &m_zs, INFLATE_WIN_BITS )) ) {
      m_error = zError(ret);
      return ret || -1;
   }

   // Read gzip header
   if ( 0 != (ret = _read_header( extra_mzgf, sizeof(extra_mzgf))) ) return ret;
   if ( extra_mzgf[0] != 'M' || extra_mzgf[1] != 'Z' ) {
      m_error = "not in MZGF format";
      return MZGF_NOT_MZGZIP;
   } else if ( MZGF_VERSION != ( m_version = extra_mzgf[4]) ) {
      m_error = "incompatible MZGF version";
      return MZGF_BAD_VERSION;
   }

   if ( 0 != (ret = _read_eof() ))    return ret;  // Read MZGF EOF block
   if ( 0 != (ret = _read_bindex() )) return ret;  // Read MZGF bindex block(s)

   // lookup file size
   struct stat stat_buf;
   int rc = stat(path, &stat_buf);
   m_zfilesize = rc == 0 ? stat_buf.st_size : -1;

   return ret;
}

void MZGFileReader::close() {
   if ( m_fp ) {
      fclose( m_fp );
      m_fp = NULL;
   }
   (void)inflateEnd( &m_zs );
}

ssize_t MZGFileReader::read( unsigned char *data, ssize_t size ) {
   if ( this->eof() ) {
      m_error = "read past end of file";
      return -1;
   }

   log_debug( "size: %d  m_zoffset: %ld m_uoffset: %ld m_boffset: %ld m_blen: %d\n",
              size, m_zoffset, m_uoffset, m_boffset, m_blen );

   ssize_t avail  = 0;
   ssize_t copied = 0;
   int     have   = 0;
   while ( copied < size ) {
      avail = m_blen - m_boffset;
      log_debug( "next avail: %d m_boffset: %ld m_blen: %d\n",
                 avail, m_boffset, m_blen );
      if ( avail <= 0 ) {
         if ( -1 == (avail = _read_block()) ) return -1;
         if ( m_blen == 0 ) break;
         avail = m_blen - m_boffset;
         if ( m_boffset > m_blen ) {      // outside of block, adjust offset
            m_boffset = m_boffset - m_blen;
            m_blen = 0;
            continue;
         }
      }

      have = (size - copied) < avail ? size - copied : avail;
log_debug( "copy avail: %d m_boffset: %d have: %d\n", avail, m_boffset, have );
      memcpy( data, m_ublock + m_boffset, have );
      data      += have;
      copied    += have;
      m_boffset += have;
      if ( m_boffset >= m_blen ) {           // on next block?
         m_blen = m_boffset = 0;
      }
   }

   return copied;
}

mzgfoff_t MZGFileReader::vtell() { return( m_zoffset ); }
off_t     MZGFileReader::tell()  { return( ftell(m_fp) ); }

int MZGFileReader::vseek( mzgfoff_t voffset ) {

   log_debug( "voffset: %ld fpos: %ld m_zoffset: %ld m_boffset: %ld\n", voffset,
              ftell(m_fp), m_zoffset, m_boffset );

   m_boffset = voffset & 0xFFFF;                // offset in uncompressed block
   off_t zoffset = voffset >> 16;               // block offset in gzip stream
   if ( zoffset == m_zoffset ) return 0;        // already there?

   if ( 0 != fseek( m_fp, zoffset, SEEK_SET ) ) {
      m_error = std::strerror(errno) ? : "unrecognized seek error";
      return errno || -1;
   }

   m_zoffset = zoffset;
   m_blen    = 0;                      // indicate current block isn't loaded
   m_isEOF   = false;                  // no longer at end of stream

   m_zs.zalloc   = Z_NULL;
   m_zs.zfree    = Z_NULL;
   m_zs.opaque   = Z_NULL;
   m_zs.next_in   = m_zblock;
   m_zs.avail_in = 0;
   m_zs.next_out  = m_ublock;
   m_zs.avail_out = MZGF_BLOCK_SIZE;
   int ret;
   if ( Z_OK != (ret = inflateInit2( &m_zs, INFLATE_WIN_BITS )) ) {
      m_error = zError(ret);
      return ret || -1;
   }
   return 0;
}

int MZGFileReader::useek( off_t uoffset ) {

   log_debug( "uoffset: %ld fpos: %ld m_zoffset: %ld m_boffset: %ld\n", uoffset,
              ftell(m_fp), m_zoffset, m_boffset );

   int ret;
   if ( !m_bindex.size() ) {
      if ( 0 != (ret = _read_bindex() )) return ret;  // Read MZGF bindex block(s)
   }

   // Binary search to find block the offset is in
   int lower = 0;
   int upper = m_bindex.size()-1;
   int mid;

   while ( lower < upper ) {
      mid = (lower+upper)/2;
      assert( mid < upper );
      if ( uoffset < m_bindex[mid].uoffset ) {
         upper = mid;
      } else {
         lower = mid + 1;
      }
   }

   if ( uoffset < m_bindex[lower].uoffset ) lower--;
log_debug( "  lower: %d  mid: %d upper: %d\n", lower, mid, upper );

   m_boffset = uoffset - m_bindex[lower].uoffset;
   if ( m_zoffset == m_bindex[lower].zoffset ) return 0;

   m_zoffset = m_bindex[lower].zoffset;
   m_uoffset = uoffset;
   m_blen    = 0;                      // indicate current block isn't loaded
   m_isEOF   = false;                  // no longer at end of stream

   log_debug( "block index %ld m_zoffset %ld m_boffset %ld\n", lower,
              m_zoffset, m_boffset );

   m_zs.zalloc   = Z_NULL;
   m_zs.zfree    = Z_NULL;
   m_zs.opaque   = Z_NULL;
   m_zs.next_in   = m_zblock;
   m_zs.avail_in = 0;
   m_zs.next_out  = m_ublock;
   m_zs.avail_out = MZGF_BLOCK_SIZE;
   if ( Z_OK != (ret = inflateInit2( &m_zs, INFLATE_WIN_BITS )) ) {
      m_error = zError(ret);
      return ret || -1;
   }

   if ( 0 != fseek( m_fp, m_bindex[lower].zoffset, SEEK_SET ) ) {
      m_error = std::strerror(errno) ? : "unrecognized seek error";
      return errno || -1;
   }

   return 0;
}


ssize_t MZGFileReader::zfilesize() {
   return m_zfilesize;
}

ssize_t MZGFileReader::ufilesize() {
   return m_ufilesize;
}

//
// Read a GZIP header according to RFC 1952 (see description above)
//
int MZGFileReader::_read_header( void *extra, int extralen ) {

   log_debug( "reading header extralen: %d\n", extralen );

   int count = fread( m_zblock, 1, sizeof(gzheader), m_fp );
   if ( ferror(m_fp) ) {
      m_error = std::strerror( errno ) ? : "unrecognized error reading header";
      return errno || -1;
   } else if ( count != sizeof(gzheader) ) {
      m_error = "read incomplete gzip header";
      return MZGF_ERR_HEADER;
   }
   m_zoffset += sizeof(gzheader);

   // Are we even a gzip file?
   if ( m_zblock[0] != GZIP_MAGIC_ID1 || m_zblock[1] != GZIP_MAGIC_ID2
        || m_zblock[2] != GZIP_CM_DEFLATED ) {
      m_error = "not in gzip format";
      return MZGF_NOT_GZIP;
   }

   m_mtime = unpackInt32( &m_zblock[4] );             // MTIME field

   // Are there extra fields?
   uint8_t  flag = m_zblock[3];                       // FLG field
   uint16_t xlen = unpackInt16( &m_zblock[10] );      // XLEN field
   if ( !(flag & GZIP_FEXTRA_FLG) && !xlen ) {
      m_error = "missing extra field(s) in gzip header";
      return MZGF_BAD_FORMAT;
   }
   else if ( xlen > extralen ) {
      m_error = "length extra fields exceeded expectation";
      return MZGF_BAD_FORMAT;
   }

   // Read in extra fields
   count = fread( extra, 1, xlen, m_fp );
   if ( ferror(m_fp) ) {
      m_error = std::strerror( errno ) ? : "unrecognized error reading extras";
      return errno || -1;
   } else if ( count != xlen ) {
      m_error = "read incomplete gzip extras";
      return MZGF_ERR_HEADER;
   }
   m_zoffset += xlen;

   return 0;
}

//
// Read in one or more block indexes.  These are empty gzip members where the
// gzip header contains the block index offsets.
//
int MZGFileReader::_read_bindex() {
   int ret   = 0;
   off_t pos = ftell(m_fp);               // save current position

   // for each block index
   int count = 0;
   off_t offset = m_bindex_offset;
   while ( offset ) {
      log_debug( "reading bindex block at offset %ld\n", offset );

      // Go to the block
      if ( 0 != fseek( m_fp, offset, SEEK_SET ) ) {
         m_error = std::strerror(errno);
         return errno || -1;
      }

      // Read gzip header w/extra field
      if ( 0 != (ret = _read_header( extra_bindex, sizeof(extra_bindex))) ) {
         return ret;
      }

      // Get block index offset extra field
      if ( extra_bindex[0] == 'B' || extra_bindex[1] == 'I' ) {
          count  = unpackInt16( &extra_bindex[2] );
          offset = unpackInt64( &extra_bindex[4] );
      } else {
          m_error = "missing MZGF block index";
          return MZGF_BAD_FORMAT;
      }

      log_debug( "bindex size: %d offset next: %ld\n", count, offset );

      bindex_t bi;
      for ( int i = 12; i < count; ) {
         bi.zoffset = unpackInt64(&extra_bindex[i]);
         i += sizeof(uint64_t);
         bi.uoffset = unpackInt64(&extra_bindex[i]);
         i += sizeof(uint64_t);
         m_bindex.push_back( bi );
      }
   }

   // restore current position
   if ( 0 != fseek( m_fp, pos, SEEK_SET ) ) {
      m_error = std::strerror(errno) ? : "unrecognized seek error";
      return errno || -1;
   }
   return 0;
}

//
// Reads the end of the file for the expected EOF member. This is a gzipped
// member of fixed size and contains pointers to other gzip members within
// the file.
//
int MZGFileReader::_read_eof() {
   int ret;
   off_t pos = ftell(m_fp);               // save current position

   // Seek to the start of the eof section found at the end of file
   int bsize = sizeof(gzheader) + sizeof(extra_eof)
             + 2     // 2 bytes for empty compressed content
             + 4     // 4 bytes for uncompressed size
             + 4;    // 4 bytes for the CRC
   if ( 0 != fseek( m_fp, -bsize, SEEK_END ) ) {
      m_error = std::strerror(errno);
      return errno || -1;
   }

   log_debug( "reading eof section at %ld\n", ftell(m_fp) );

   // Read gzip header w/extra field
   if ( 0 != (ret = _read_header( extra_eof, sizeof(extra_eof))) ) return ret;

   // Parse extra field containing block index offset and size
   if ( extra_eof[0] == 'B' || extra_eof[1] == 'O' ) {
      m_ufilesize     = unpackInt64( &extra_eof[4] );
      m_bindex_offset = unpackInt64( &extra_eof[12] );
   } else {
      m_error = "missing MZGF block index offset";
      return MZGF_BAD_FORMAT;
   }
   log_debug( "bindex_offset: %ld\n",  m_bindex_offset );

   // restore current position
   if ( 0 != fseek( m_fp, pos, SEEK_SET ) ) {
      m_error = std::strerror(errno) ? : "unrecognized seek error";
      return errno || -1;
   }

   return 0;
}

//
// Read in a gzipped block
//
ssize_t MZGFileReader::_read_block() {

   m_zs.next_out  = m_ublock;
   m_zs.avail_out = MZGF_BLOCK_SIZE;

   // read a block of compressed bytes
   if ( m_zs.avail_in == 0 ) {            // need more input?
      log_debug( "fpos: %ld zoffset: %ld\n", ftell(m_fp), m_zoffset );
      m_zs.next_in  = m_zblock;
      m_zs.avail_in = fread( m_zblock, 1, MZGF_BLOCK_SIZE, m_fp );
      if ( m_zs.avail_in != MZGF_BLOCK_SIZE && ferror( m_fp ) ) {
         m_error = std::strerror(errno) ? : "error reading compressed stream";
         return -1;
      } else if ( m_zs.avail_in == 0 ) {
         m_error = "read past end of file";
         return -1;
      }
      m_zoffset += m_zs.avail_in;
   }

   log_debug( "inflate beg avail_in: %-5d avail_out: %-5d\n", m_zs.avail_in,
              m_zs.avail_out );

   m_zs.next_out  = m_ublock;
   m_zs.avail_out = MZGF_BLOCK_SIZE;

   switch ( int ret = ::inflate( &m_zs, Z_BLOCK ) ) {
      case Z_OK :
         break;
      case Z_STREAM_END :
         m_isEOF = true;
         break;
      case Z_STREAM_ERROR :
         m_error = zError( ret );
std::cerr << m_error << std::endl; exit(2);
         return -1;
         break;
      default :
         m_error = zError(ret);
std::cerr << m_error << std::endl; exit(1);
         return -1;
   }

   m_blen     = MZGF_BLOCK_SIZE - m_zs.avail_out;
   m_uoffset += m_blen;
   m_isEOF    = m_isEOF | (feof(m_fp) && (m_zs.avail_in <= 8));

   log_debug( "inflate end avail_in: %-5d avail_out: %-5d m_blen: %d\n",
              m_zs.avail_in, m_zs.avail_out, m_blen );
   return m_blen;
}






//
// Inflate the block in m_zblock into m_ublock
//
int MZGFileReader::_inflate_block() {
    int ret = 0;

    m_zs.next_in   = m_zblock;
    m_zs.avail_in  = MZGF_BLOCK_SIZE;
    m_zs.next_out  = m_ublock;
    m_zs.avail_out = MZGF_BLOCK_SIZE;

    if ( Z_STREAM_END != (ret = ::inflate( &m_zs, Z_FINISH ) ) ) {
       m_error = zError( ret );
       return ret || -1;
    }
    return m_zs.total_out;
}


/*
int MZGFileReader::read_block() {

   m_zs.next_in   = m_zblock;
   m_zs.next_out  = m_ublock;
   m_zs.avail_out = MZGF_BLOCK_SIZE;

   int count = fread( m_zblock, 1, MZGF_BLOCK_SIZE, m_fp );
   if ( count == 0 ) { // no data read
      if ( feof(m_fp) ) return 1;
std::cerr << "read error" << std::endl;
      this->block_length = 0;
      exit(1);
      return 0;
   }
std::cerr << "read in " << count << std::endl;
   m_zs.avail_in  = count;

int ret, have;
do {
   m_zs.next_out  = m_ublock;
   m_zs.avail_out = MZGF_BLOCK_SIZE;

   ret  = inflate( &m_zs, Z_BLOCK );
   have = MZGF_BLOCK_SIZE - m_zs.avail_out;
std::cerr << "        ret: " << ret << std::endl;
std::cerr << "       have: " << have << std::endl;
std::cerr << "   avail_in: " << m_zs.avail_in << std::endl;
std::cerr << "  avail_out: " << m_zs.avail_out << std::endl;
   assert( ret != Z_STREAM_ERROR );
switch (ret) {
case Z_NEED_DICT:
    ret = Z_DATA_ERROR;
/*
case Z_DATA_ERROR:
case Z_MEM_ERROR:
//    (void)inflateEnd(&strm);
    return ret;
}
if ( have ) {
//m_ublock[have-1] = NULL;
//std::cerr << (char *)m_ublock << std::endl;
}
} while ( m_zs.avail_out == 0 );

   return 0;
}
*/

//
//int MZGFileReader::inflate( FILE *dst ) {
//
//   assert( compressBound(MZGF_BLOCK_SIZE) < MZGF_MAX_BLOCK_SIZE );
//   assert( m_fp );
//
//   ssize_t have;
//   while ( !this->eof() ) {
//      if ( 0 > (have = this->_read_block()) ) {
//         return -1;
//      }
//      if ( (fwrite( m_ublock, 1, have, dst ) != have) || ferror(dst) ) {
//         m_error = std::strerror(errno) ? : "unrecognized error writing block";
//         return -1;
//      }
//   }
//
//   return 0;
//}

}                 // end of MZGFile namespace

// -*- mode: C++; tab-width: 4; c-basic-offset: 4 -*-
// vi: set expandtab ts=4 sw=4 sts=4:
