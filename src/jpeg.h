#ifndef JPEG_H_
#define JPEG_H_

typedef unsigned const char* BufPtr;

// When JPEG is stored as a file it is wrapped in a container
// This function fixes up the provided start ptr to point to the
// actual JPEG stream data and returns the number of bytes skipped
// returns true if the file seems to be valid jpeg
// If quant tables can be found they will be stored in qtable0/1
bool decodeJPEGfile(BufPtr *start, uint32_t *len, BufPtr *qtable0, BufPtr *qtable1);
bool findJPEGheader(BufPtr *start, uint32_t *len, uint8_t marker);

// Given a jpeg ptr pointing to a pair of length bytes, advance the pointer to
// the next 0xff marker byte
void nextJpegBlock(BufPtr *start);

#endif