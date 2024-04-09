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

EMURat::EMURat(EMUDataset *pDS, const std::shared_ptr<std::mutex>& other)
{
    m_pEMUDS = pDS;
    m_mutex = other;
    m_nRowCount = 0;
}

EMURat::~EMURat()
{
    
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

CPLErr EMURat::ValuesIO(GDALRWFlag eRWFlag, int iField, int iStartRow, int iLength, double *pdfData)
{
    if( (iField < 0) || (iField > m_cols.size()))
    {
        CPLError(CE_Failure, CPLE_FileIO,
                "Couldn't find column %d.",
                iField);
        return CE_Failure;
    }
    
    // just do this for now, rather than complex conversions that KEA does
    if( m_cols[iField].colType != GFT_Real ) 
    {
        CPLError(CE_Failure, CPLE_FileIO,
                "Wrong type for column %d, expected double.",
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
        vsi_l_offset chunkOffset = VSIFTellL(m_pEMUDS->m_fp);
        VSIFWriteL(pdfData, sizeof(double), iLength, m_pEMUDS->m_fp);
        EMURatChunk chunk;
        chunk.startIdx = iStartRow;
        chunk.length = iLength;
        chunk.offset = chunkOffset;
        m_cols[iField].chunks.push_back(chunk);
    }
    else
    {
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
    
    // just do this for now, rather than complex conversions that KEA does
    if( m_cols[iField].colType != GFT_Integer ) 
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                "Wrong type for column %d, expected double.",
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
    int64_t *pn64Tmp = new int64_t[iLength];
    
    const std::lock_guard<std::mutex> lock(*m_mutex);
    if( eRWFlag == GF_Write) 
    {
        if(m_pEMUDS->GetAccess() != GA_Update)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
            "The EMU driver only supports writing when creating");
            return CE_Failure;
        }
        
        vsi_l_offset chunkOffset = VSIFTellL(m_pEMUDS->m_fp);
        // convert
        for( int i = 0; i < iLength; i++ ) 
        {
            pn64Tmp[i] = pnData[i];
        }
        
        VSIFWriteL(pn64Tmp, sizeof(int64_t), iLength, m_pEMUDS->m_fp);
        EMURatChunk chunk;
        chunk.startIdx = iStartRow;
        chunk.length = iLength;
        chunk.offset = chunkOffset;
        m_cols[iField].chunks.push_back(chunk);
    }
    else
    {
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
                "Wrong type for column %d, expected double.",
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
        
        vsi_l_offset chunkOffset = VSIFTellL(m_pEMUDS->m_fp);
        for( int i = 0; i < iLength; i++ )
        {
            VSIFWriteL(papszStrList[i], sizeof(char), strlen(papszStrList[i]) + 1, m_pEMUDS->m_fp);
        }
        
        EMURatChunk chunk;
        chunk.startIdx = iStartRow;
        chunk.length = iLength;
        chunk.offset = chunkOffset;
        m_cols[iField].chunks.push_back(chunk);
    }
    else
    {
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
    
