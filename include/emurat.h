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

struct EMURatChunk
{
    uint64_t startIdx;
    uint64_t length;
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
    EMURat(EMUDataset *pDS, const std::shared_ptr<std::mutex>& other);
    ~EMURat();

    virtual int           GetColumnCount() const override;
    virtual const char   *GetNameOfCol( int ) const override;
    virtual GDALRATFieldUsage GetUsageOfCol( int ) const override;
    virtual GDALRATFieldType GetTypeOfCol( int ) const override;
    
    virtual int           GetColOfUsage( GDALRATFieldUsage ) const override;

    virtual int           GetRowCount() const override;
    
private:

    EMUDataset *m_pEMUDS;
    std::shared_ptr<std::mutex> m_mutex;
    std::vector<EMURatColumn> m_cols;

};

#endif //EMURAT_H
 