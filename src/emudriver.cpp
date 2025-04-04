/*
 *  emudriver.cpp
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

// needs a separate file so GDAL_COMPILATION can be defined
// so CPL_DLL etc gets definted properly...
#define GDAL_COMPILATION
#include "emudataset.h"

CPL_C_START
void CPL_DLL GDALRegister_EMU(void);
CPL_C_END

void GDALRegister_EMU()
{
    if( !GDAL_CHECK_VERSION("EMU") )
        return;

    if( GDALGetDriverByName("EMU") != nullptr )
        return;
    
    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("EMU");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "UBARSC Streaming Format (.emu)");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSIONS, "emu");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATECOPY, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_CREATIONDATATYPES, 
            "Byte Int8 Int16 UInt16 Int32 UInt32 Int64 UInt64 Float32 Float64");

    poDriver->pfnOpen = EMUDataset::Open;
    poDriver->pfnIdentify = EMUDataset::Identify;
    poDriver->pfnCreate = EMUDataset::Create;
    poDriver->pfnCreateCopy = EMUDataset::CreateCopy;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
