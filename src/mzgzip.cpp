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

#include <assert.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <ctime>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>

#include "MZGFile.h"
#include "zlib.h"

using namespace MZGFile;

// Globals
const std::string prog("mzgzip");      // program name
std::string opt_file;                  // name of file to operate on
bool opt_stdout     = false;           // force overwriting output file
bool opt_force      = false;           // force overwriting output file
bool opt_decompress = false;           // decompress input stream
mzgfoff_t opt_voffset = -1;            // virtual offset to start decompress at
off_t     opt_uoffset = -1;            // uncompressed offset to decompress at
ssize_t   opt_size    = SSIZE_MAX;     // number of bytes to decompress
bool opt_list = false;                 // list contents


//
// Print usage to STDOUT and exit
//
static void printUsage() {
   std::cout << "usage: mzgzip [options] [mzML file|mzML.mgz file]" << std::endl;
   std::cout << "Compress or decompress mzML input" << std::endl << std::endl;
   std::cout << "Options" << std::endl;
   std::cout << "   -h, help        give this help" << std::endl;
   std::cout << "   -c              write to standard output, keep orig files unchanged" << std::endl;
   std::cout << "   -f, force       overwrite files without asking" << std::endl;
   std::cout << "   -d, decompress  decompress" << std::endl;
   std::cout << "   -l, list        list compressed file contents" << std::endl;
   std::cout << "   -v, voffset INT decompress at virtual file pointer INT" << std::endl;
   std::cout << "   -u, uoffset INT decompress at INT bytes into uncompressed file" << std::endl;
   std::cout << "   -s, size INT    decompress up to INT bytes" << std::endl;
   std::cout << std::endl;
}

//
// Parse command line options
//
static void parseOptions( int argc, char **argv ) {

   static struct option opts[] = {
      { "help",       no_argument, NULL, 'h' },
      { "force",      no_argument, NULL, 'f' },
      { "decompress", no_argument, NULL, 'd' },
      { "uoffset",    required_argument, NULL, 'u' },
      { "voffset",    required_argument, NULL, 'v' },
      { "size",       required_argument, NULL, 's' },
      { "list",       no_argument, NULL, 'l' }
   };

   int c, oidx = 0;
   while ( (c = getopt_long( argc, argv, "hcfdv:u:s:l", opts, &oidx )) != -1 ) {
      switch ( c ) {
         case 'h' :
            printUsage();
            exit(0);
            break;
         case 'c' :
            opt_stdout = true;
            break;
         case 'f' :
            opt_force = true;
            break;
         case 'd' :
            opt_decompress = true;
            break;
         case 'u' :
            opt_uoffset = atoll(optarg);
            opt_decompress = true;
            break;
         case 'v' :
            opt_voffset = atoll(optarg);
            opt_decompress = true;
            break;
         case 's' :
            opt_size = atoll(optarg);
            opt_decompress = true;
            break;
         case 'l' :
            opt_list = true;
            break;
         default :
            printUsage();
            exit(0);
      }
   }

   if ( (argc - optind) != 1 )  {
      std::cerr << prog << ": wrong number of arguments" << std::endl;
      printUsage();
      exit(1);
   }

   opt_file = argv[optind];
}

//
// Compress contents of input file
//
int compress( std::string file ) {
   int ret = 0;

   // Open input
   FILE *src_fh;
   if ( NULL == (src_fh = fopen( file.c_str(), "rb" )) ) {
      std::cerr << prog << ": " << file << ": " << strerror(errno) << std::endl;
      return -1;
   }

   // Open output
   FILE *dst_fh = stdout;
   if ( !opt_stdout ) {
      // ...named correctly?
      if ( file.substr( file.length()-4 ) == ".mgz" ) {
         std::cerr << prog << ": " << file;
         std::cerr << " already has .mgz suffix -- unchanged";
         std::cerr << std::endl;
         return -1;
      }

      // ...already exist?
      struct stat sbuf;
      file += ".mgz";
      if ( 0 == stat( file.c_str(), &sbuf ) && !opt_force ) {
         char c;
         std::cout << prog << ": " << file;
         std::cout << " already exists; do you wish to overwrite (y or n)? ";
         std::cin >> c;
         if ( c != 'Y' && c != 'y' ) {
            std::cerr << prog << ": not overwritten" << std::endl;
            return -1;
         }
      }

      /// ...open it
      if ( NULL == (dst_fh = fopen( file.c_str(), "wb") ) ) {
         std::cerr << prog << ": " << file << ": " << strerror(errno);
         fclose( src_fh );
         return -1;
      }
   }

   // Compress contents
   MZGFileWriter w;
   if ( 0 != (ret = w.deflate( src_fh, dst_fh )) ) {
      std::cerr << prog << ": " << file << ": " << w.strerror() << std::endl;
   }

   // Done
   fclose( src_fh );
   if ( !opt_stdout )
      fclose( dst_fh );

   return ret;
}

//
// Decompress a input stream.  At the moment we can only use a file as input
// since we need to seek() to read the index(s).
//
int decompress( std::string file ) {
   MZGFileReader r;
   int ret = 0;

   // Open input
   FILE *src_fh;
   if ( file.substr( file.length()-4 ) != ".mgz" ) {
      std::cerr << prog << ": " << file << " unknown suffix -- ignored";
      std::cerr << std::endl;
      return -1;
   }
   if ( r.open( file.c_str() ) ) {
      std::cerr << prog << ": " << file << ": " << r.strerror() << std::endl;
      return -1;
   }

   // Open output
   FILE *dst_fh = stdout;
   if ( !opt_stdout ) {
      file.resize( file.length()-4 );        // remove .mgz extension
      struct stat sbuf;
      if ( 0 == stat( file.c_str(), &sbuf ) && !opt_force ) {
         char c;
         std::cout << prog << ": " << file;
         std::cout << " already exists; do you wish to overwrite (y or n)? ";
         std::cin >> c;
         if ( c != 'Y' && c != 'y' ) {
            std::cerr << prog << ": not overwritten" << std::endl;
            return -1;
         }
      }
      if ( 0 == (dst_fh = fopen( file.c_str(), "wb") ) ) {
         std::cerr << prog << ": " << file << ": " << std::strerror(errno);
         std::cerr << std::endl;
         return -1;
      }
   }

   if ( opt_voffset != -1 ) {             // Start at virtual position
      ret = r.vseek( opt_voffset );
   } else if ( opt_uoffset != -1 ) {      // Start at uncompressed position
      ret = r.useek( opt_uoffset );
   }

   if ( -1 != ret ) {
      unsigned char buffer[MZGF_BLOCK_SIZE];
      int have;
      while ( opt_size > 0 && !r.eof() ) {
            have = opt_size < MZGF_BLOCK_SIZE ? opt_size : MZGF_BLOCK_SIZE;
            if ( -1 == (ret = have = r.read( buffer, have)) ) break;
            fwrite( buffer, 1, have, dst_fh );
            opt_size -= have;
      }
   }

   if ( -1 == ret ) {
      std::cerr << prog << ": " << file << ": " << std::strerror(errno);
      std::cerr << std::endl;
   }

   // Done
   r.close();
   if ( !opt_stdout )
      fclose( dst_fh );

   return ret;
}

//
// List contents of a compressed file.
//
int contents( std::string file ) {
   MZGFileReader r;
   int ret = 0;

   // Open input
   FILE *src_fh;
   if ( file.substr( file.length()-4 ) != ".mgz" ) {
      std::cerr << prog << ": " << file << " unknown suffix -- ignored";
      std::cerr << std::endl;
      return -1;
   }
   if ( r.open( file.c_str() ) ) {
      std::cerr << prog << ": " << file << ": " << r.strerror() << std::endl;
      return -1;
   }

   std::cout << "MZGF Version: " << (int)r.version() << std::endl;
   time_t mtime = r.mtime();
   std::cout << "MZGF Date Time: " << asctime(localtime(&mtime));
   std::cout << "MZGF Uncompressed size: " << r.ufilesize() << std::endl;
   std::cout << "MZGF Virtual/Uncompressed Offsets:" << std::endl;
   std::vector<bindex_t> bindex = r.bindex();
   for ( int i = 0; i < bindex.size(); i++ ) {
      std::cout << std::setw(14) << (bindex[i].zoffset << 16);
      std::cout << " ";
      std::cout << std::setw(12) << bindex[i].uoffset;
      std::cout << std::endl;
   }
   return 0;
}

//
// -- MAIN ---------------------------------------------------------------------
//
int main( int argc, char **argv )
{
    parseOptions( argc, argv );

    int ret;
    if ( opt_list ) {
       ret = contents( opt_file );
    } else if ( opt_decompress ) {
       ret = decompress( opt_file );
    } else {
       ret = compress( opt_file );
    }
    exit(ret);
}
