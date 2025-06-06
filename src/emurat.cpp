/*
 *  emurat.cpp
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
 
#include "emurat.h"
#include "emuband.h"
#include "emucompress.h"

#include <algorithm> 


EMURat::EMURat(EMUDataset *pDS, EMURasterBand *pBand, const std::shared_ptr<std::mutex>& other)
{
    m_pEMUDS = pDS;
    m_pEMUBand = pBand;
    m_mutex = other;
    m_nRowCount = 0;
}

EMURat::~EMURat()
{
    
}

GDALDefaultRasterAttributeTable *EMURat::Clone() const
{
    CPLError(CE_Failure, CPLE_FileIO,
            "Cloning RAT not yet supported");
    return nullptr;
}

int EMURat::GetColumnCount() const
{
    return m_cols.size();
}

const char* EMURat::GetNameOfCol( int nCol) const
{
    if( (nCol < 0) || (nCol > m_cols.size()))
    {
        return nullptr;
    }
    return m_cols[nCol].sName.c_str();
}

GDALRATFieldUsage EMURat::GetUsageOfCol( int nCol ) const
{
    if( (nCol < 0) || (nCol > m_cols.size()))
    {
        return GFU_Generic;
    }
    
    GDALRATFieldUsage eGDALUsage = GFU_Generic;
    if( m_cols[nCol].sName == "Histogram")
    {
        eGDALUsage = GFU_PixelCount;
    }
    else if( m_cols[nCol].sName == "Name")
    {
        eGDALUsage = GFU_Name;
    }
    else if( m_cols[nCol].sName == "Red")
    {
        eGDALUsage = GFU_Red;
    }
    else if( m_cols[nCol].sName == "Green")
    {
        eGDALUsage = GFU_Green;
    }
    else if( m_cols[nCol].sName == "Blue")
    {
        eGDALUsage = GFU_Blue;
    }
    else if( m_cols[nCol].sName == "Alpha")
    {
        eGDALUsage = GFU_Alpha;
    }
    return eGDALUsage;
}

GDALRATFieldType EMURat::GetTypeOfCol( int nCol ) const
{
    if( (nCol < 0) || (nCol > m_cols.size()))
    {
        return GFT_Integer;
    }
    
    return m_cols[nCol].colType;
}

int EMURat::GetColOfUsage( GDALRATFieldUsage eUsage ) const
{
    std::string emuusage;
    switch(eUsage)
    {
    case GFU_PixelCount:
        emuusage = "PixelCount";
        break;
    case GFU_Name:
        emuusage = "Name";
        break;
    case GFU_Red:
        emuusage = "Red";
        break;
    case GFU_Green:
        emuusage = "Green";
        break;
    case GFU_Blue:
        emuusage = "Blue";
        break;
    case GFU_Alpha:
        emuusage = "Alpha";
        break;
    default:
        emuusage = "Generic";
        break;
    }
    for( int i = 0; i < m_cols.size(); i++ )
    {
        if( m_cols[i].sName == emuusage )
            return i;
    }
    return -1;
}


int EMURat::GetRowCount() const
{
    return m_nRowCount;
}

const char *EMURat::GetValueAsString( int iRow, int iField ) const
{
    // Get ValuesIO do do the work
    char *apszStrList[1];
    if( (const_cast<EMURat*>(this))->
                ValuesIO(GF_Read, iField, iRow, 1, apszStrList ) != CPLE_None )
    {
        return "";
    }

    const_cast<EMURat*>(this)->osWorkingResult = apszStrList[0];
    CPLFree(apszStrList[0]);

    return osWorkingResult;
}

int EMURat::GetValueAsInt( int iRow, int iField ) const
{
    // Get ValuesIO do do the work
    int nValue;
    if( (const_cast<EMURat*>(this))->
                ValuesIO(GF_Read, iField, iRow, 1, &nValue ) != CE_None )
    {
        return 0;
    }

    return nValue;
}

double EMURat::GetValueAsDouble( int iRow, int iField ) const
{
    // Get ValuesIO do do the work
    double dfValue;
    if( (const_cast<EMURat*>(this))->
                ValuesIO(GF_Read, iField, iRow, 1, &dfValue ) != CE_None )
    {
        return 0;
    }

    return dfValue;
}

void EMURat::SetValue( int iRow, int iField, const char *pszValue )
{
    // Get ValuesIO do do the work
    ValuesIO(GF_Write, iField, iRow, 1, const_cast<char**>(&pszValue) );
}

void EMURat::SetValue( int iRow, int iField, double dfValue)
{
    // Get ValuesIO do do the work
    ValuesIO(GF_Write, iField, iRow, 1, &dfValue );
}

void EMURat::SetValue( int iRow, int iField, int nValue )
{
    // Get ValuesIO do do the work
    ValuesIO(GF_Write, iField, iRow, 1, &nValue );
}


CPLErr EMURat::ValuesIO(GDALRWFlag eRWFlag, int iField, int iStartRow, int iLength, double *pdfData)
{
    if( (iField < 0) || (iField > m_cols.size()))
    {
        CPLError(CE_Failure, CPLE_FileIO,
                "Couldn't find column %d.",
                iField);
        return CE_Failure;
    }

    // do simple conversions of type
    if( m_cols[iField].colType == GFT_Integer )
    {
        if(eRWFlag == GF_Write)
        {
            int *pnData = (int*)CPLMalloc(iLength * sizeof(int));
            for( int n = 0; n < iLength; n++)
            {
                pnData[n] = pdfData[n];
            }
            CPLErr err = ValuesIO(eRWFlag, iField, iStartRow, iLength, pnData);
            CPLFree(pnData);
            return err;
        }
        else
        {
            int *pnData = (int*)CPLMalloc(iLength * sizeof(int));
            CPLErr err = ValuesIO(eRWFlag, iField, iStartRow, iLength, pnData);
            if( err != CE_None)
            {
                for( int n = 0; n < iLength; n++)
                {
                    pdfData[n] = pnData[n];
                }
            }
            CPLFree(pnData);
            return err;
        }
    }    
    else if( m_cols[iField].colType == GFT_String ) 
    {
        CPLError(CE_Failure, CPLE_FileIO,
                "Wrong type for column %d, expected number, got string.",
                iField);
        return CE_Failure;
    }
    
    if( iStartRow >= m_nRowCount )
    {
        return CE_None;   
    }
    else if( (iStartRow + iLength) > m_nRowCount )
    {
        iLength = m_nRowCount - iStartRow;
    }

    
    const std::lock_guard<std::mutex> lock(*m_mutex);
    if( eRWFlag == GF_Write) 
    {
        if(m_pEMUDS->GetAccess() != GA_Update)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
            "The EMU driver only supports writing when creating");
            return CE_Failure;
        }

        // TODO: set compression
        uint8_t compression = COMPRESSION_ZLIB;
        int iThisBlockLength;
        
        while(iLength > 0)
        {
            if( iLength > MAX_RAT_CHUNK)
            {
                iThisBlockLength = MAX_RAT_CHUNK;
            }
            else
            {
                iThisBlockLength = iLength;
            }
            vsi_l_offset chunkOffset = VSIFTellL(m_pEMUDS->m_fp);
            
            VSIFWriteL(&compression, sizeof(compression), 1, m_pEMUDS->m_fp);
            size_t uncompressedSize = iThisBlockLength * sizeof(double);
            size_t compressedSize = uncompressedSize + 100;

            bool bFree;
            Bytef *pCompressed = doCompression(compression, reinterpret_cast<Bytef*>(pdfData), uncompressedSize, &compressedSize, &bFree);
                    
            VSIFWriteL(pCompressed, compressedSize, 1, m_pEMUDS->m_fp);
            
            if( bFree ) 
            {
                CPLFree(pCompressed);
            }
            
            EMURatChunk chunk;
            chunk.startIdx = iStartRow;
            chunk.length = iThisBlockLength;
            chunk.offset = chunkOffset;
            chunk.compressedSize = compressedSize;
            m_cols[iField].chunks.push_back(chunk);
            
            pdfData += iThisBlockLength;
            iLength -= iThisBlockLength;
            iStartRow += iThisBlockLength;
        }
    }
    else
    {
        bool bFound = false;
        size_t startChunk = 0;
        uint64_t nElsToSkipAtStart = 0;
        uint64_t nCopiedRows = 0;
        for( size_t n = 0; n < m_cols[iField].chunks.size(); n++)
        {
            if(m_cols[iField].chunks[n].startIdx <= iStartRow )
            {
                startChunk = n;
                nElsToSkipAtStart = iStartRow - m_cols[iField].chunks[n].startIdx;
                bFound = true;
                break;
            }
        }
        
        if( bFound )
        {
            while( (nCopiedRows <  iLength) && (startChunk < m_cols[iField].chunks.size()) )
            {
                // seek to where this block starts    
                VSIFSeekL(m_pEMUDS->m_fp, m_cols[iField].chunks[startChunk].offset, SEEK_SET);
    
                // read compression type
                uint8_t compression;
                VSIFReadL(&compression, sizeof(compression), 1, m_pEMUDS->m_fp);
    
                // read the uncompressed data
                Bytef *pSubData = static_cast<Bytef*>(CPLMalloc(m_cols[iField].chunks[startChunk].compressedSize));
                VSIFReadL(pSubData, m_cols[iField].chunks[startChunk].compressedSize, 1, m_pEMUDS->m_fp);
                
                uint64_t uncompressedSize =  m_cols[iField].chunks[startChunk].length * sizeof(double); 
                Bytef *pUncompressed = static_cast<Bytef*>(CPLMalloc(uncompressedSize)); 
                doUncompression(compression, pSubData, m_cols[iField].chunks[startChunk].compressedSize,
                    pUncompressed, uncompressedSize);
                double *pData = reinterpret_cast<double*>(pUncompressed);
                
                // copy out
                uint64_t count = 0;
                while( (nCopiedRows < iLength) && (count < m_cols[iField].chunks[startChunk].length) ) 
                {
                    pdfData[nCopiedRows] = pData[nElsToSkipAtStart];
                    nCopiedRows++;
                    nElsToSkipAtStart++;
                    count++;
                }
                
                CPLFree(pSubData);
                CPLFree(pUncompressed);
                startChunk++;
                nElsToSkipAtStart = 0;
            }
            
            while( nCopiedRows < iLength )
            {
                // we haven't been able to copy all the data they have requested 
                // because that much data was never written. Pad with zeros.
                pdfData[nCopiedRows] = 0;
                nCopiedRows++;
            }
            
        }
        
        
    
    }
    
    
    return CE_None;
}

CPLErr EMURat::ValuesIO(GDALRWFlag eRWFlag, int iField, int iStartRow, int iLength, int *pnData)
{
    if( (iField < 0) || (iField > m_cols.size()))
    {
        CPLError(CE_Failure, CPLE_FileIO,
                "Couldn't find column %d.",
                iField);
        return CE_Failure;
    }
    
    // do simple conversions of type
    if( m_cols[iField].colType == GFT_Real )
    {
        if(eRWFlag == GF_Write)
        {
            double *pfdData = (double*)CPLMalloc(iLength * sizeof(double));
            for( int n = 0; n < iLength; n++)
            {
                pfdData[n] = pnData[n];
            }
            CPLErr err = ValuesIO(eRWFlag, iField, iStartRow, iLength, pfdData);
            CPLFree(pfdData);
            return err;
        }
        else
        {
            double *pfdData = (double*)CPLMalloc(iLength * sizeof(double));
            CPLErr err = ValuesIO(eRWFlag, iField, iStartRow, iLength, pfdData);
            if( err != CE_None)
            {
                for( int n = 0; n < iLength; n++)
                {
                    pnData[n] = pfdData[n];
                }
            }
            CPLFree(pfdData);
            return err;
        }
    }    
    else if( m_cols[iField].colType == GFT_String ) 
    {
        CPLError(CE_Failure, CPLE_FileIO,
                "Wrong type for column %d, expected number, got string.",
                iField);
        return CE_Failure;
    }

    
    if( iStartRow >= m_nRowCount )
    {
        return CE_None;   
    }
    else if( (iStartRow + iLength) > m_nRowCount )
    {
        iLength = m_nRowCount - iStartRow;
    }    
    
    // we store as int64 internally
    int64_t *pn64Tmp = new int64_t[MAX_RAT_CHUNK];

    const std::lock_guard<std::mutex> lock(*m_mutex);
    if( eRWFlag == GF_Write) 
    {
        if(m_pEMUDS->GetAccess() != GA_Update)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
            "The EMU driver only supports writing when creating");
            return CE_Failure;
        }

        // TODO: set compression
        uint8_t compression = COMPRESSION_ZLIB;
        int iThisBlockLength;
        
        while(iLength > 0)
        {
            if( iLength > MAX_RAT_CHUNK)
            {
                iThisBlockLength = MAX_RAT_CHUNK;
            }
            else
            {
                iThisBlockLength = iLength;
            }

            vsi_l_offset chunkOffset = VSIFTellL(m_pEMUDS->m_fp);
            // convert
            for( int i = 0; i < iThisBlockLength; i++ ) 
            {
                pn64Tmp[i] = pnData[i];
            }

            VSIFWriteL(&compression, sizeof(compression), 1, m_pEMUDS->m_fp);
            size_t uncompressedSize = iThisBlockLength * sizeof(int64_t);
            size_t compressedSize = uncompressedSize + 100;

            bool bFree;
            Bytef *pCompressed = doCompression(compression, reinterpret_cast<Bytef*>(pn64Tmp), uncompressedSize, &compressedSize, &bFree);
                    
            VSIFWriteL(pCompressed, compressedSize, 1, m_pEMUDS->m_fp);
            
            if( bFree ) 
            {
                CPLFree(pCompressed);
            }

            EMURatChunk chunk;
            chunk.startIdx = iStartRow;
            chunk.length = iThisBlockLength;
            chunk.offset = chunkOffset;
            chunk.compressedSize = compressedSize;
            m_cols[iField].chunks.push_back(chunk);

            pnData += iThisBlockLength;
            iLength -= iThisBlockLength;
            iStartRow += iThisBlockLength;
        }
    }
    else
    {
        bool bFound = false;
        size_t startChunk = 0;
        uint64_t nElsToSkipAtStart = 0;
        uint64_t nCopiedRows = 0;
        for( size_t n = 0; n < m_cols[iField].chunks.size(); n++)
        {
            if(m_cols[iField].chunks[n].startIdx <= iStartRow )
            {
                startChunk = n;
                nElsToSkipAtStart = iStartRow - m_cols[iField].chunks[n].startIdx;
                bFound = true;
                break;
            }
        }
        
        if( bFound )
        {
            while( (nCopiedRows <  iLength) && (startChunk < m_cols[iField].chunks.size()) )
            {
                // seek to where this block starts    
                VSIFSeekL(m_pEMUDS->m_fp, m_cols[iField].chunks[startChunk].offset, SEEK_SET);
    
                // read compression type
                uint8_t compression;
                VSIFReadL(&compression, sizeof(compression), 1, m_pEMUDS->m_fp);
    
                // read the uncompressed data
                Bytef *pSubData = static_cast<Bytef*>(CPLMalloc(m_cols[iField].chunks[startChunk].compressedSize));
                VSIFReadL(pSubData, m_cols[iField].chunks[startChunk].compressedSize, 1, m_pEMUDS->m_fp);
                
                uint64_t uncompressedSize =  m_cols[iField].chunks[startChunk].length * sizeof(int); 
                Bytef *pUncompressed = static_cast<Bytef*>(CPLMalloc(uncompressedSize)); 
                doUncompression(compression, pSubData, m_cols[iField].chunks[startChunk].compressedSize,
                    pUncompressed, uncompressedSize);
                // stored as int64 internally
                int64_t *pData = reinterpret_cast<int64_t*>(pUncompressed);
                
                // copy out
                uint64_t count = 0;
                while( (nCopiedRows < iLength) && (count < m_cols[iField].chunks[startChunk].length) ) 
                {
                    pnData[nCopiedRows] = pData[nElsToSkipAtStart];
                    nCopiedRows++;
                    nElsToSkipAtStart++;
                    count++;
                }
                
                CPLFree(pSubData);
                CPLFree(pUncompressed);
                startChunk++;
                nElsToSkipAtStart = 0;
            }
            
            while( nCopiedRows < iLength )
            {
                // we haven't been able to copy all the data they have requested 
                // because that much data was never written. Pad with zeros.
                pnData[nCopiedRows] = 0;
                nCopiedRows++;
            }
            
        }
        
        
    
    }

    
    delete[] pn64Tmp;
    
    return CE_None;
    
}


CPLErr EMURat::ValuesIO(GDALRWFlag eRWFlag, int iField, int iStartRow, int iLength, char **papszStrList)
{
    if( (iField < 0) || (iField > m_cols.size()))
    {
        CPLError(CE_Failure, CPLE_FileIO,
                "Couldn't find column %d.",
                iField);
        return CE_Failure;
    }
    
    // just do this for now, rather than complex conversions that KEA does
    if( m_cols[iField].colType != GFT_String ) 
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                "Wrong type for column %d, expected string.",
                iField);
        return CE_Failure;
    }
    
    if( iStartRow >= m_nRowCount )
    {
        return CE_None;   
    }
    else if( (iStartRow + iLength) > m_nRowCount )
    {
        iLength = m_nRowCount - iStartRow;
    }

    const std::lock_guard<std::mutex> lock(*m_mutex);
    if( eRWFlag == GF_Write) 
    {
        if(m_pEMUDS->GetAccess() != GA_Update)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
            "The EMU driver only supports writing when creating");
            return CE_Failure;
        }

        // TODO: set compression
        uint8_t compression = COMPRESSION_ZLIB;
        int iThisBlockLength;

        while(iLength > 0)
        {
            if( iLength > MAX_RAT_CHUNK)
            {
                iThisBlockLength = MAX_RAT_CHUNK;
            }
            else
            {
                iThisBlockLength = iLength;
            }
        
            vsi_l_offset chunkOffset = VSIFTellL(m_pEMUDS->m_fp);
            
            // work out the total size of all the strings (including null byte)
            // so we can compress them all in one go
            size_t totalStringSize = 0;
            for( int i = 0; i < iThisBlockLength; i++ )
            {
                totalStringSize += strlen(papszStrList[i]) + 1;
            }
            
            char *pszStringData = new char[totalStringSize];
            char *pszCurrentDest = pszStringData;
            for( int i = 0; i < iThisBlockLength; i++ )
            {
                pszCurrentDest = stpcpy(pszCurrentDest, papszStrList[i]) + 1;
            }
            
            VSIFWriteL(&compression, sizeof(compression), 1, m_pEMUDS->m_fp);
            size_t uncompressedSize = totalStringSize;
            size_t compressedSize = uncompressedSize + 100;

            bool bFree;
            Bytef *pCompressed = doCompression(compression, reinterpret_cast<Bytef*>(pszStringData), uncompressedSize, &compressedSize, &bFree);
                    
            VSIFWriteL(pCompressed, compressedSize, 1, m_pEMUDS->m_fp);
            
            if( bFree ) 
            {
                CPLFree(pCompressed);
            }
            
            delete[] pszStringData;
            
            EMURatChunk chunk;
            chunk.startIdx = iStartRow;
            chunk.length = iThisBlockLength;
            chunk.offset = chunkOffset;
            chunk.compressedSize = compressedSize;
            m_cols[iField].chunks.push_back(chunk);

            papszStrList += iThisBlockLength;
            iLength -= iThisBlockLength;
            iStartRow += iThisBlockLength;
        }
    }
    else
    {
        bool bFound = false;
        size_t startChunk = 0;
        uint64_t nElsToSkipAtStart = 0;
        uint64_t nCopiedRows = 0;
        for( size_t n = 0; n < m_cols[iField].chunks.size(); n++)
        {
            if(m_cols[iField].chunks[n].startIdx <= iStartRow )
            {
                startChunk = n;
                nElsToSkipAtStart = iStartRow - m_cols[iField].chunks[n].startIdx;
                bFound = true;
                break;
            }
        }
        
        if( bFound )
        {
            while( (nCopiedRows <  iLength) && (startChunk < m_cols[iField].chunks.size()) )
            {
                // seek to where this block starts    
                VSIFSeekL(m_pEMUDS->m_fp, m_cols[iField].chunks[startChunk].offset, SEEK_SET);
    
                // read compression type
                uint8_t compression;
                VSIFReadL(&compression, sizeof(compression), 1, m_pEMUDS->m_fp);
    
                // read the uncompressed data
                Bytef *pSubData = static_cast<Bytef*>(CPLMalloc(m_cols[iField].chunks[startChunk].compressedSize));
                VSIFReadL(pSubData, m_cols[iField].chunks[startChunk].compressedSize, 1, m_pEMUDS->m_fp);
                
                uint64_t uncompressedSize =  m_cols[iField].chunks[startChunk].length * sizeof(int); 
                Bytef *pUncompressed = static_cast<Bytef*>(CPLMalloc(uncompressedSize)); 
                doUncompression(compression, pSubData, m_cols[iField].chunks[startChunk].compressedSize,
                    pUncompressed, uncompressedSize);
                char *pszStringData = reinterpret_cast<char*>(pUncompressed);
                char *pszCurrentSrc = pszStringData;
                while( nElsToSkipAtStart > 0 )
                {
                    // go to next string
                    pszCurrentSrc += (strlen(pszCurrentSrc) + 1);
                    nElsToSkipAtStart--;
                }
                
                // copy out
                uint64_t count = 0;
                while( (nCopiedRows < iLength) && (count < m_cols[iField].chunks[startChunk].length) ) 
                {
                    papszStrList[nCopiedRows] = CPLStrdup(pszCurrentSrc);
                    // go to next string
                    pszCurrentSrc += (strlen(pszCurrentSrc) + 1);
                    nCopiedRows++;
                    count++;
                }
                
                CPLFree(pSubData);
                CPLFree(pUncompressed);
                startChunk++;
            }
            
            while( nCopiedRows < iLength )
            {
                // we haven't been able to copy all the data they have requested 
                // because that much data was never written. Pad with empty string.
                papszStrList[nCopiedRows] = CPLStrdup("");
                nCopiedRows++;
            }
            
        }
    }
    
    
    return CE_None;
}

int EMURat::ChangesAreWrittenToFile()
{
    if(m_pEMUDS->GetAccess() == GA_Update)
    {
        return TRUE;
    }
    else
    {
        return FALSE;    
    }
}

void EMURat::SetRowCount( int iCount )
{
    if( iCount > m_nRowCount )
    {
        m_nRowCount = iCount;
    }
}

CPLErr EMURat::SetTableType(const GDALRATTableType eInTableType)
{
    // TODO: should we do something better here?
    return CE_None;
}

GDALRATTableType EMURat::GetTableType() const
{
    return GRTT_THEMATIC;
}

void EMURat::RemoveStatistics()
{
    // should I be clearing the stats on the band here??
}


CPLErr EMURat::CreateColumn( const char *pszFieldName, 
                                GDALRATFieldType eFieldType, 
                                GDALRATFieldUsage eFieldUsage )
{
    EMURatColumn col;
    col.sName = pszFieldName;
    col.colType = eFieldType;
    m_cols.push_back(col);
    return CE_None;
}

bool chunkSortFunction(const EMURatChunk &a, const EMURatChunk &b)
{
    return a.startIdx < b.startIdx;
}

void EMURat::ReadIndex()
{
    uint64_t nCols;
    VSIFReadL(&nCols, sizeof(nCols), 1, m_pEMUDS->m_fp);
    VSIFReadL(&m_nRowCount, sizeof(m_nRowCount), 1, m_pEMUDS->m_fp);

    m_cols.clear();
    for( int i = 0; i < nCols; i++ )
    {
        EMURatColumn col;
        uint64_t nType;
        VSIFReadL(&nType, sizeof(nType), 1, m_pEMUDS->m_fp);
        col.colType = static_cast<GDALRATFieldType>(nType);
        
        std::string name;
        char ch;
        while(true)
        {
            VSIFReadL(&ch, sizeof(ch), 1, m_pEMUDS->m_fp);
            if( ch == '\0')
            {
                break;
            }
            else
            {
                name += ch;
            }
        }
        col.sName = name;
        
        uint64_t nChunks;
        VSIFReadL(&nChunks, sizeof(nChunks), 1, m_pEMUDS->m_fp);
        for( int n = 0; n < nChunks; n++)
        {
            EMURatChunk chunk;
            VSIFReadL(&chunk.startIdx, sizeof(chunk.startIdx), 1, m_pEMUDS->m_fp);
            VSIFReadL(&chunk.length, sizeof(chunk.length), 1, m_pEMUDS->m_fp);
            VSIFReadL(&chunk.offset, sizeof(chunk.offset), 1, m_pEMUDS->m_fp);
            VSIFReadL(&chunk.compressedSize, sizeof(chunk.compressedSize), 1, m_pEMUDS->m_fp);
            col.chunks.push_back(chunk);
        }
        
        m_cols.push_back(col);
    }
}

void EMURat::WriteIndex()
{
    uint64_t nCols = m_cols.size();
    VSIFWriteL(&nCols, sizeof(nCols), 1, m_pEMUDS->m_fp);
    VSIFWriteL(&m_nRowCount, sizeof(m_nRowCount), 1, m_pEMUDS->m_fp);
     
    for( int i = 0; i < m_cols.size(); i++ )
    {
        // sort first
        std::sort(m_cols[i].chunks.begin(), m_cols[i].chunks.end(), chunkSortFunction);
        uint64_t nType = m_cols[i].colType;
        VSIFWriteL(&nType, sizeof(nType), 1, m_pEMUDS->m_fp);
        
        const char *pszName = m_cols[i].sName.c_str();
        while( *pszName != '\0' )
        {
            VSIFWriteL(pszName, sizeof(char), 1, m_pEMUDS->m_fp);
            pszName++;
        }
        VSIFWriteL(pszName, sizeof(char), 1, m_pEMUDS->m_fp); // null byte
        
        uint64_t nChunks = m_cols[i].chunks.size();
        VSIFWriteL(&nChunks, sizeof(nChunks), 1, m_pEMUDS->m_fp);
        
        for( auto itr = m_cols[i].chunks.begin(); itr != m_cols[i].chunks.end(); itr++ )
        {
            VSIFWriteL(&(*itr).startIdx, sizeof((*itr).startIdx), 1, m_pEMUDS->m_fp);
            VSIFWriteL(&(*itr).length, sizeof((*itr).length), 1, m_pEMUDS->m_fp);
            VSIFWriteL(&(*itr).offset, sizeof((*itr).offset), 1, m_pEMUDS->m_fp);
            VSIFWriteL(&(*itr).compressedSize, sizeof((*itr).compressedSize), 1, m_pEMUDS->m_fp);
        }
    }
}
