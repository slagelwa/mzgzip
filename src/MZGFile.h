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

#ifndef MZGFILE_H
#define MZGFILE_H

#include <assert.h>
#include <getopt.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <iostream>
#include <vector>

#include "zlib.h"


#define MZGF_VERSION    1              // MZGF format version (max 255)
#define MZGF_BLOCK_SIZE 0xff00         // Size of uncompressed blocks (64K)
#define MZGF_MAX_BLOCK_SIZE 0x10000    // Limit on block size

#define MZGF_FERROR        0x1         // I/O error occurred
#define MZGF_NOT_GZIP      0x3         // Not in gzip format
#define MZGF_NOT_MZGZIP    0x4         // Not in mzgzip format
#define MZGF_ERR_HEADER    0x5         // GZIP header error occurred
#define MZGF_BAD_FORMAT    0x6         // MZGF format problem
#define MZGF_BAD_VERSION   0x7         // MZGF version is not recognized

namespace MZGFile {

// Virtual offset in a MGZFile.  Comprised of an 6 byte block address into the
// compressed stream and a 2 byte offset into the uncompressed block
typedef int64_t mzgfoff_t;

typedef unsigned char byte_t;

// Block indices
typedef struct bindex {
   uint64_t zoffset;          // offset of block in compressed stream
   uint64_t uoffset;          // offset of block in uncompressed stream
} bindex_t;

class MZGFileWriter {

   uint8_t  m_version;                       // what MZGF version are we
   time_t   m_mtime;                         // mtime
   FILE     *m_fp;                           // compressed file handle
   z_stream m_zs;                            // zlib stream

   byte_t   m_ublock[MZGF_BLOCK_SIZE];       // uncompressed block
   off_t    m_uoffset;                       // uncompressed offset
   uint32_t m_ucrc32;                        // uncompressed crc32 checksum
   off_t    m_usize;                         // uncompressed size

   byte_t m_zblock[MZGF_BLOCK_SIZE];         // compressed block
   off_t m_zoffset;                          // current compressed/block offset

   std::vector<bindex_t> m_bindex;           // block index
   off_t m_bindex_offset;                    // index to start of next bindex

   std::string m_error;                      // description for any error

   int _write_header( uint8_t *extra = NULL, int extralen = 0 );
   int _write_block( int );
   int _flush();
   int _write_empty();
   int _write_trailer();
   int _write_bindex();
   int _write_eof();

public :
   MZGFileWriter();
   ~MZGFileWriter();

   /**
    * Reads all of the input from the source, deflates it and writes it out
    * to the destination. Sets strerror() on error with a description of the
    * error.
    *
    * @param      Source file handle
    * @param      Destination file handle
    * @return     0 on success and -1 on error
    */
   int deflate( FILE *src, FILE *dst );

   /**
    * Returns a string describing any error condition.
    *
    * @return     Description of the error
    */
   std::string strerror() { return m_error; };

};

 // ----------------------------------------------------------------------------
 // ----------------------------------------------------------------------------
 // ----------------------------------------------------------------------------

class MZGFileReader {

   FILE     *m_fp;                           // compressed file handle
   z_stream m_zs;                            // zlib stream
   uint8_t  m_version;                       // what MZGF version are we
   time_t   m_mtime;                         // mtime
   bool     m_isEOF;                         // reached end of input?
   ssize_t  m_zfilesize;                     // size of the compressed file
   ssize_t  m_ufilesize;                     // size of the uncompressed file

   byte_t m_zblock[MZGF_BLOCK_SIZE];         // compressed block
   off_t m_zoffset;                          // compressed (block) offset
   byte_t m_ublock[MZGF_BLOCK_SIZE];         // uncompressed block
   off_t  m_uoffset;                         // uncompressed offset
   int    m_blen;                            // length of uncompressed block
   int    m_boffset;                         // offset into uncompressed block

   std::vector<bindex_t> m_bindex;           // block index
   off_t m_bindex_offset;                    // offset of next bindex block

   std::string m_error;                      // description for any error

   int _read_header( void *extra = NULL, int extralen = 0 );
   ssize_t _read_block();
   int _inflate_block();
   int _read_bindex();
   int _read_eof();

public :
   MZGFileReader();

   uint8_t version() { return m_version; };
   time_t  mtime()   { return m_mtime; };
   std::vector<bindex_t> &bindex() { return m_bindex; };

   /**
    * Open the specified file for reading.
    *
    * @param   name of file to open
    * @return  returns nonzero on error and sets strerror()
    */
   int open( const char * );

   /**
    * Closes the file associated with the MZGFReader.
    *
    * @return  Non-zero value on error and sets strerror()
    */
   void close();

   /**
	 * Read up to _count bytes from the compressed stream storing into _data_.
    *
    * @param data    Data array to read into
    * @param count   Number of bytes to read
    * @return        Number of bytes actually read or on error -1 is returned
    *                and strerror() is set with an error description
    */
   ssize_t read( unsigned char *data, ssize_t count );

   /**
    * Checks whether the end of file has been reached.
    *
    * @return  A non-zero value in the case that the end of file has been
    *          reached.
    */
   inline int eof() { return m_isEOF; };

   /**
    * Return the virtual position in the current compressed stream.
    * No interpretation of the value should be made, other than a subsequent
    * call to seek can be used to position the file at the same point.
    *
    * @return  Current virtual offset or on error -1 is returned and
    *          strerror() is set with an error description.
    */
   mzgfoff_t vtell();

   /**
    * Positions the reader at _offset_ from the beginning of the compressed
    * data.
    *
    * @param   Virtual file offset, returned by vtell()
    * @return  Returns zero if successful else it returns a non-zero value
    *          and strerror() is set with an error description.
    */
   int vseek( mzgfoff_t );

   /**
     * Positions the reader at _offset_ from the beginning of the *uncompressed*
     * stream by using the block index appended to the stream to position to
     * the correct virtual offset.
     *
     * @param  File offset, returned by tell()
     * @return Returns zero if successful else it returns a non-zero value
     *         and strerror() is set with an error description.
     */
    int useek( off_t );

    /**
     * Returns the current position in the stream.
     *
     * @return  Current offset or on error -1 is returned and
     *          strerror() is set with an error description.
     */
    off_t tell();

   /**
    * @return  Size of the compressed file
    */
   ssize_t zfilesize();

   /**
    * @return  Size of the uncompressed file
    */
   ssize_t ufilesize();

   /**
    * Returns a string describing any error condition.
    *
    * @return     Description of the error
    */
   std::string strerror() { return m_error; };

};       // end class MZGFileReader

}        // namespace MZGFile

#endif   // ifndef MZGFILE_H

// -*- mode: C++; tab-width: 4; c-basic-offset: 4 -*-
// vi: set expandtab ts=4 sw=4 sts=4:
