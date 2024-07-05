/*
 *  emurat.h
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

#ifndef EMURAT_H
#define EMURAT_H

#include <string>
#include <vector>
#include <mutex>

#include "gdal_rat.h"

#include "emudataset.h"

class EMURasterBand;

struct EMURatChunk
{
    uint64_t startIdx;
    uint64_t length;
    uint64_t offset;
    uint64_t compressedSize;
};

struct EMURatColumn
{
    std::string sName;
    GDALRATFieldType colType;
    std::vector<EMURatChunk> chunks;
};

class EMURat final: public GDALRasterAttributeTable
{
public:
    EMURat(EMUDataset *pDS, EMURasterBand *pBand, const std::shared_ptr<std::mutex>& other);
    ~EMURat();

    virtual GDALDefaultRasterAttributeTable *Clone() const override;
    virtual int           GetColumnCount() const override;
    virtual const char   *GetNameOfCol( int ) const override;
    virtual GDALRATFieldUsage GetUsageOfCol( int ) const override;
    virtual GDALRATFieldType GetTypeOfCol( int ) const override;
    
    virtual int           GetColOfUsage( GDALRATFieldUsage ) const override;

    virtual int           GetRowCount() const override;

    virtual const char   *GetValueAsString( int iRow, int iField ) const;
    virtual int           GetValueAsInt( int iRow, int iField ) const;
    virtual double        GetValueAsDouble( int iRow, int iField ) const;

    virtual void          SetValue( int iRow, int iField, const char *pszValue );
    virtual void          SetValue( int iRow, int iField, double dfValue);
    virtual void          SetValue( int iRow, int iField, int nValue );
    
    virtual CPLErr        ValuesIO(GDALRWFlag eRWFlag, int iField, int iStartRow, int iLength, double *pdfData) override;
    virtual CPLErr        ValuesIO(GDALRWFlag eRWFlag, int iField, int iStartRow, int iLength, int *pnData) override;
    virtual CPLErr        ValuesIO(GDALRWFlag eRWFlag, int iField, int iStartRow, int iLength, char **papszStrList) override;
    
    virtual int           ChangesAreWrittenToFile() override;
    virtual void          SetRowCount( int iCount ) override;
    virtual CPLErr SetTableType(const GDALRATTableType eInTableType) override;
    virtual GDALRATTableType GetTableType() const override;
    virtual void          RemoveStatistics() override;

    virtual CPLErr        CreateColumn( const char *pszFieldName, 
                                GDALRATFieldType eFieldType, 
                                GDALRATFieldUsage eFieldUsage ) override;
    void ReadIndex();

private:

    void WriteIndex();

    EMUDataset *m_pEMUDS;
    EMURasterBand *m_pEMUBand;
    std::shared_ptr<std::mutex> m_mutex;
    std::vector<EMURatColumn> m_cols;
    uint64_t m_nRowCount;
    CPLString osWorkingResult;
};

#endif //EMURAT_H
 