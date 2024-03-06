

#include "gdal_pam.h"

#include <mutex>
#include <thread>
#include <map>

class EMUDataset final: public GDALPamDataset
{
public:
    EMUDataset(VSILFILE *);
    ~EMUDataset();

    static GDALDataset *Open( GDALOpenInfo * );
    static int Identify( GDALOpenInfo * );
    static GDALDataset *Create(const char * pszFilename,
                                int nXSize, int nYSize, int nBands,
                                GDALDataType eType,
                                char **);

    CPLErr GetGeoTransform( double * padfTransform ) override;
    const OGRSpatialReference* GetSpatialRef() const override;

private:
    VSILFILE  *m_fp = nullptr;
    OGRSpatialReference m_oSRS{};
    
    friend class EMURasterBand;
};

EMUDataset::EMUDataset(VSILFILE *fp)
{
    m_fp = fp;
}

EMUDataset::~EMUDataset()
{
    VSIClose(m_fp);
}

GDALDataset *EMUDataset::Open(GDALOpenInfo *poOpenInfo)
{
    return nullptr;
}

int EMUDataset::Identify(GDALOpenInfo *poOpenInfo)
{
    return nullptr;
}

GDALDataset *EMUDataset::Create(const char * pszFilename,
                                int nXSize, int nYSize, int nBands,
                                GDALDataType eType,
                                char ** /* papszParamList */)
{
    // Try to create the file.
    FILE *fp = VSIFOpen(pszFilename, "w");
    if( fp == NULL )
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                "Attempt to create file `%s' failed.",
                pszFilename);
        return NULL;
    }
    
    VSIFWrite("EMU", 4, 1, fp);
    
    return new EMUDataset(fp);
}


class EMURasterBand final: public GDALPamRasterBand
{
public:
    EMURasterBand(EMUDataset *, int);
    ~EMURasterBand();
    
    virtual CPLErr IReadBlock( int, int, void * ) override;
    virtual CPLErr IWriteBlock( int, int, void * ) override;
    
private:
    friend class EMUDataset;
    EMUDataset *m_pDataset;
};

EMURasterBand::EMURasterBand(EMUDataset *pDataset, int)
{
    m_pDataset = pDataset;
}


CPLErr EMURasterBand::IReadBlock(int, int, void *)
{
    return CE_Failure;
}

CPLErr EMURasterBand::IWriteBlock(int, int, void *)
{
    return CE_Failure;
}

#ifdef MSVC
    #define EMU_EXPORT __declspec(dllexport)
#else
    #define EMU_EXPORT
#endif

EMU_EXPORT void GDALRegister_EMU()
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

    poDriver->pfnOpen = EMUDataset::Open;
    poDriver->pfnIdentify = EMUDataset::Identify;
    poDriver->pfnCreate = EMUDataset::Create;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
