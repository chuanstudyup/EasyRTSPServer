#include <assert.h>
#include <Arduino.h>
#include "jpeg.h"

// search for a particular JPEG marker, moves *start to just after that marker
// This function fixes up the provided start ptr to point to the
// actual JPEG stream data and returns the number of bytes skipped
// APP0 e0
// DQT db
// DQT db
// DHT c4
// DHT c4
// DHT c4
// DHT c4
// SOF0 c0 baseline (not progressive) 3 color 0x01 Y, 0x21 2h1v, 0x00 tbl0
// - 0x02 Cb, 0x11 1h1v, 0x01 tbl1 - 0x03 Cr, 0x11 1h1v, 0x01 tbl1
// therefore 4:2:2, with two separate quant tables (0 and 1)
// SOS da
// EOI d9 (no need to strip data after this RFC says client will discard)
bool findJPEGheader(BufPtr *start, uint32_t *len, uint8_t marker) {
    // per https://en.wikipedia.org/wiki/JPEG_File_Interchange_Format
    unsigned const char *bytes = *start;

    // kinda skanky, will break if unlucky and the headers inxlucde 0xffda
    // might fall off array if jpeg is invalid
    // FIXME - return false instead
    while(bytes - *start < *len) {
        uint8_t framing = *bytes++; // better be 0xff
        if(framing != 0xff) {
            Serial.printf("malformed jpeg, framing=%x\n", framing);
            return false;
        }
        uint8_t typecode = *bytes++;
        if(typecode == marker) {
            unsigned skipped = bytes - *start;
            //if ( debug ) printf("found marker 0x%x, skipped %d\n", marker, skipped);

            *start = bytes;

            // shrink len for the bytes we just skipped
            *len -= skipped;

            return true;
        }
        else {
            // not the section we were looking for, skip the entire section
            switch(typecode) {
            case 0xd8:     // start of image
            {
                break;   // no data to skip
            }
            case 0xe0:   // app0
            case 0xdb:   // dqt
            case 0xc4:   // dht
            case 0xc0:   // sof0
            case 0xda:   // sos
            {
                // standard format section with 2 bytes for len.  skip that many bytes
                uint32_t len = bytes[0] * 256 + bytes[1];
                //if ( debug ) printf("skipping section 0x%x, %d bytes\n", typecode, len);
                bytes += len;
                break;
            }
            default:
                Serial.printf("unexpected jpeg typecode 0x%x\n", typecode);
                break;
            }
        }
    }

    Serial.printf("failed to find jpeg marker 0x%x", marker);
    return false;
}

// the scan data uses byte stuffing to guarantee anything that starts with 0xff
// followed by something not zero, is a new section.  Look for that marker and return the ptr
// pointing there
void skipScanBytes(BufPtr *start) {
    BufPtr bytes = *start;

    while(true) { // FIXME, check against length
        while(*bytes++ != 0xff);
        if(*bytes++ != 0) {
            *start = bytes - 2; // back up to the 0xff marker we just found
            return;
        }
    }
}
void  nextJpegBlock(BufPtr *bytes) {
    uint32_t len = (*bytes)[0] * 256 + (*bytes)[1];
    //if ( debug ) printf("going to next jpeg block %d bytes\n", len);
    *bytes += len;
}

// When JPEG is stored as a file it is wrapped in a container
// This function fixes up the provided start ptr to point to the
// actual JPEG stream data and returns the number of bytes skipped
bool decodeJPEGfile(BufPtr *start, uint32_t *len, BufPtr *qtable0, BufPtr *qtable1) {
    // per https://en.wikipedia.org/wiki/JPEG_File_Interchange_Format
    unsigned const char *bytes = *start;

    if(!findJPEGheader(&bytes, len, 0xd8)) // better at least look like a jpeg file
        return false; // FAILED!

    // Look for quant tables if they are present
    *qtable0 = NULL;
    *qtable1 = NULL;
    BufPtr quantstart = *start;
    uint32_t quantlen = *len;
    if(!findJPEGheader(&quantstart, &quantlen, 0xdb)) {
        Serial.printf("error can't find quant table 0\n");
    }
    else {
        // if ( debug ) printf("found quant table %x\n", quantstart[2]);

        *qtable0 = quantstart + 3;     // 3 bytes of header skipped
        nextJpegBlock(&quantstart);
        if(!findJPEGheader(&quantstart, &quantlen, 0xdb)) {
            Serial.printf("error can't find quant table 1\n");
        }
        else {
            // if ( debug ) printf("found quant table %x\n", quantstart[2]);
        }
        *qtable1 = quantstart + 3;
        nextJpegBlock(&quantstart);
    }

    if(!findJPEGheader(start, len, 0xda))
        return false; // FAILED!

    // Skip the header bytes of the SOS marker FIXME why doesn't this work?
    uint32_t soslen = (*start)[0] * 256 + (*start)[1];
    *start += soslen;
    *len -= soslen;

    // start scanning the data portion of the scan to find the end marker
    BufPtr endmarkerptr = *start;
    uint32_t endlen = *len;

    skipScanBytes(&endmarkerptr);
    if(!findJPEGheader(&endmarkerptr, &endlen, 0xd9))
        return false; // FAILED!

    // endlen must now be the # of bytes between the start of our scan and
    // the end marker, tell the caller to ignore bytes afterwards
    *len = endmarkerptr - *start;

    return true;
}