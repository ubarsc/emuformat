/*
 *  emucompress.cpp
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
 
 #include "gdal_pam.h"

 #include "emucompress.h"

// https://gist.github.com/arq5x/5315739
Bytef* doCompression(int type, Bytef *pInput, size_t inputSize, size_t *pnOutputSize, bool *pbFree) 
{
    if( type == COMPRESSION_NONE )
    {
        // do nothing
        *pnOutputSize = inputSize;
        *pbFree = false;
        return pInput;
    }
    else if( type == COMPRESSION_ZLIB )
    {
        Bytef *pOutput = static_cast<Bytef*>(CPLMalloc(inputSize));
    
        z_stream defstream;
        defstream.zalloc = Z_NULL;
        defstream.zfree = Z_NULL;
        defstream.opaque = Z_NULL;
        defstream.avail_in = inputSize;
        defstream.next_in = pInput;
        defstream.avail_out = inputSize;
        defstream.next_out = pOutput;
        
        // the actual compression work.
        deflateInit(&defstream, Z_BEST_COMPRESSION);
        deflate(&defstream, Z_FINISH);
        deflateEnd(&defstream);
        
        *pnOutputSize = defstream.total_out;
        *pbFree = true;
        return pOutput;
    } 
    else
    {
        fprintf(stderr, "Unknown compression type\n");
        return nullptr;
    }
}


void doUncompression(uint8_t type, Bytef *pInput, size_t inputSize, Bytef *pOutput, size_t pnOutputSize)
{
    if( type == COMPRESSION_NONE )
    {
        memcpy(pOutput, pInput, inputSize);
    }
    else if( type == COMPRESSION_ZLIB )
    {
        z_stream infstream;
        infstream.zalloc = Z_NULL;
        infstream.zfree = Z_NULL;
        infstream.opaque = Z_NULL;
        infstream.avail_in = inputSize;
        infstream.next_in = pInput;
        infstream.avail_out = pnOutputSize;
        infstream.next_out = pOutput;
         
        // the actual DE-compression work.
        inflateInit(&infstream);
        inflate(&infstream, Z_NO_FLUSH);
        inflateEnd(&infstream);
    } 
    else
    {
        fprintf(stderr, "Unknown compression type\n");
    }

}

Bytef* doCompressMetadata(int type, char **papszMetadataList, size_t *pnInputSize, size_t *pnOutputSize)
{
    size_t nInputSize = 0;
    int nIndex = 0;
    while( papszMetadataList[nIndex] != nullptr )
    {
        nInputSize += (strlen(papszMetadataList[nIndex]) + 1);
        nIndex++;
    }
    
    // another one for the double null at the end
    nInputSize++;
    
    char *pData = static_cast<char*>(CPLMalloc(nInputSize));
    char *pPos = pData;
    nIndex = 0;
    while( papszMetadataList[nIndex] != nullptr )
    {
        fprintf(stderr, "metadata %s\n", papszMetadataList[nIndex]);
        pPos = stpcpy(pPos, papszMetadataList[nIndex]);
        pPos++;
        nIndex++;
    }
    *pPos = '\0';
    
    for( int i = 0; i < nInputSize; i++)
    {
        fprintf(stderr, "%c,", pData[i]);
    }
    fprintf(stderr, "\n");
    
    bool bFree;
    Bytef *pResult = doCompression(type, reinterpret_cast<Bytef*>(pData), nInputSize, pnOutputSize, &bFree);
    *pnInputSize = nInputSize;

    for( int i = 0; i < nInputSize; i++)
    {
        fprintf(stderr, "%c,", pResult[i]);
    }
    fprintf(stderr, "\n");
    
    if( bFree )
    {
        CPLFree(pData);
    }
    return pResult;
}

char** doUncompressMetadata(uint8_t type, Bytef *pInput, size_t inputSize, size_t pnOutputSize)
{
    char **pszResult = nullptr;
    
    // first uncompress into a buffer
    char *pData = static_cast<char*>(CPLMalloc(pnOutputSize));
    doUncompression(type, pInput, inputSize, reinterpret_cast<Bytef*>(pData), pnOutputSize);
    
    // convert into a string array
    char *pPos = pData;
    while( *pPos != '\0')
    {
        fprintf(stderr, "pPos = %s\n", pPos);
        char *pNextPos = pPos + strlen(pPos) + 1;
        char *pEqualPos = strchr(pPos, '=');
        *pEqualPos = '\0';
        pszResult = CSLSetNameValue(pszResult, pPos, pEqualPos + 1);
        pPos = pNextPos;
    }
    
    CPLFree(pData);
    return pszResult;
}
