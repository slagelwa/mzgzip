# mzgzip

This repository contains a prototype utility, mzgzip for compressing mass spectrography data stored in the HUPO-PSI mzML format (or any text format for that matter) and a modified version of the mzParser to efficiently read said compressed filed.  Files compressed with mzgzip should be usable by an utility/tool that support gzip.

MZGF utilizes a similar approach to compression as the Blocked GNU Zip Format (BGZF) used bgzip/tabix tools found in htslib.  Both approaches leverage the feature of the gzip that allows multiple compressed streams to be concatenated together.  They both break the original data into blocks and compress each block separately.  Since each stream is independent of each other, this allows for more more efficient random reading as the entire file's content doesn't have to be decompressed, only the block(s) requested.  However this does come at a cost of having slightly larger files. 

MZGF adds one additional twist to the BGZF approach -- the block index itself is stored within the compressed file by appending even more compressed streams of zero size with the index stored in parts in the the optional extra headers of the gzip stream.  In this way MZGF keeps the block index intrinsically associated with the blocked compressed data, unlike with the BGZF approach.  Future implementations could use this very same approach to append more than one index to file, such as encoding the actual mzML index to spectrum, to altogether avoid having to uncompress any extraneous streams to do a random read from the file.

## See Also

* [BGZF - Blocked, Bigger & Better GZIP!](http://blastedbio.blogspot.com/2011/11/bgzf-blocked-bigger-better-gzip.html)
* [Samtools/htslib](http://www.htslib.org/doc/tabix.html)

## Notes

The BGZF library was originally implemented by Bob Handsaker and modified by Heng Li for remote file access and in-memory caching.  The mzgzip utility was modeled off of code from both this library and the example code for using the gzib library.
