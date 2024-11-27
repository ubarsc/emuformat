/*
 *  emucompress.h
 *  EMUFormat
 *
 *  Created by Sam Gillingham on 26/03/2024.
 *  Copyright 2024 EMUFormat. All rights reserved.
 *
 *  This file is part of EMUFormat.
 *
 *  Permission is hereby granted, free of charge, to any person 
 *  obtaining a copy of this software and associated documentation 
 *  files (the "Software"), to deal in the Software without restriction, 
 *  including without limitation the rights to use, copy, modify, 
 *  merge, publish, distribute, sublicense, and/or sell copies of the 
 *  Software, and to permit persons to whom the Software is furnished 
 *  to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be 
 *  included in all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 *  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES 
 *  OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 *  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR 
 *  ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF 
 *  CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION 
 *  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef EMUCOMPRESS_H
#define EMUCOMPRESS_H

#include <cstdint>
#include "zlib.h"
 
const uint8_t COMPRESSION_NONE = 0;
const uint8_t COMPRESSION_ZLIB = 1;
 
Bytef* doCompression(int type, Bytef *pInput, size_t inputSize, size_t *pnOutputSize, bool *pbFree); 
void doUncompression(uint8_t type, Bytef *pInput, size_t inputSize, Bytef *pOutput, size_t pnOutputSize);

Bytef* doCompressMetadata(int type, char **papszMetadataList, size_t *pnInputSize, size_t *pnOutputSize);
char** doUncompressMetadata(uint8_t type, Bytef *pInput, size_t inputSize, size_t pnOutputSize);

#endif //EMUCOMPRESS_H
