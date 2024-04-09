/*
 *  emuband.h
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

#ifndef EMUBAND_H
#define EMUBAND_H

#include <map>
#include "gdal_priv.h"

#include "emudataset.h"

class EMURasterBand final: public GDALRasterBand
{
public:
    EMURasterBand(EMUDataset *, int nBandIn, GDALDataType eType, bool bThematic, const std::shared_ptr<std::mutex>& other);
    ~EMURasterBand();
    
    virtual CPLErr IReadBlock( int, int, void * ) override;
    virtual CPLErr IWriteBlock( int, int, void * ) override;

    virtual double GetNoDataValue(int *pbSuccess = nullptr) override;
    virtual int64_t GetNoDataValueAsInt64(int *pbSuccess = nullptr) override;
    virtual uint64_t GetNoDataValueAsUInt64(int *pbSuccess = nullptr) override;
    virtual CPLErr SetNoDataValue(double dfNoData) override;
    virtual CPLErr SetNoDataValueAsInt64(int64_t nNoData) override;
    virtual CPLErr SetNoDataValueAsUInt64(uint64_t nNoData) override;
    virtual CPLErr DeleteNoDataValue() override;
    
    virtual double GetMinimum(int *pbSuccess) override;
    virtual double GetMaximum(int *pbSuccess) override;
    virtual CPLErr ComputeRasterMinMax(int bApproxOK, double *adfMinMax) override;
    virtual CPLErr GetStatistics(int bApproxOK, int bForce, double *pdfMin,
                                 double *pdfMax, double *pdfMean,
                                 double *padfStdDev) override;
    virtual CPLErr ComputeStatistics(int bApproxOK, double *pdfMin, double *pdfMax,
        double *pdfMean, double *pdfStdDev,	GDALProgressFunc pfnProgress,
        void *pProgressData) override;
	virtual CPLErr SetStatistics(double dfMin, double dfMax, double dfMean,	
	    double dfStdDev) override;
	    
	virtual CPLErr SetMetadataItem(const char *pszName, const char *pszValue, 
	   const char *pszDomain) override;
    virtual const char *GetMetadataItem (const char *pszName, 
        const char *pszDomain) override;
    virtual char** GetMetadata(const char *pszDomain) override;
    virtual CPLErr SetMetadata(char **papszMetadata, const char *pszDomain) override;
	     		
    bool GetThematic() { return m_bThematic;};
private:

    void AccumulateData(void *pData, size_t nLength, size_t nXValid);
    template<class T>
    void AccumulateDataForType(void *pData, size_t nLength, size_t nXValid);
    void EstimateStatsFromHistogram();
    void SetMetadataFromStats();
    void UpdateMetadataList();


    bool m_bNoDataSet;
    int64_t m_nNoData;
    bool m_bThematic;
    std::map<uint32_t, uint32_t> m_histogram;
    std::shared_ptr<std::mutex> m_mutex;
    char               **m_papszMetadataList; // CPLStringList of metadata
    
    double m_dMin;
    double m_dMax;
    double m_dMean;
    double m_dStdDev;
    double m_dMedian;
    double m_dMode;

    friend class EMUDataset;
};
#endif //EMUBAND_H
