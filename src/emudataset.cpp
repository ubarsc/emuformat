

#include "gdal_pam.h"

#include <mutex>
#include <thread>
#include <unordered_map>

const int COMPRESSION_NONE = 0;
const int COMPRESSION_ZLIB = 1;

const int EMU_VERSION = 1;

struct EMUTileKey
{
    uint64_t ovrLevel;  // 0 for full res
    uint64_t band;
    uint64_t x;
    uint64_t y;
    
    bool operator==(const EMUTileKey &other) const
    {
        return (ovrLevel == other.ovrLevel
            && band == other.band
            && x == other.x && y == other.y);
    }
};

template <>
struct std::hash<EMUTileKey>
{
    std::size_t operator()(const EMUTileKey& k) const
    {
        std::size_t h1 = std::hash<uint64_t>{}(k.ovrLevel);
        std::size_t h2 = std::hash<uint64_t>{}(k.band);
        std::size_t h3 = std::hash<uint64_t>{}(k.x);
        std::size_t h4 = std::hash<uint64_t>{}(k.y);
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
    }
};

struct EMUTileValue
{
    vsi_l_offset offset;
    uint64_t size;
};

class EMUDataset final: public GDALDataset
{
public:
    EMUDataset(VSILFILE *, GDALDataType eType, int nXSize, int nYSize, GDALAccess eInAccess);
    ~EMUDataset();

    static GDALDataset *Open( GDALOpenInfo * );
    static int Identify( GDALOpenInfo * );
    static GDALDataset *Create(const char * pszFilename,
                                int nXSize, int nYSize, int nBands,
                                GDALDataType eType,
                                char **);

    CPLErr GetGeoTransform( double * padfTransform ) override;
    CPLErr SetGeoTransform( double * padfTransform ) override;
    const OGRSpatialReference* GetSpatialRef() const override;
    CPLErr SetSpatialRef(const OGRSpatialReference*) override;
    CPLErr Close() override;
    
private:
    void setTileOffset(uint64_t o, uint64_t band, uint64_t x, 
        uint64_t y, vsi_l_offset offset, uint64_t size);
    EMUTileValue getTileOffset(uint64_t o, uint64_t band, uint64_t x, uint64_t y);


    VSILFILE  *m_fp = nullptr;
    OGRSpatialReference m_oSRS{};
    std::unordered_map<EMUTileKey, EMUTileValue> m_tileOffsets;
    double m_padfTransform[6];
    uint64_t m_tileSize;
    std::mutex m_mutex;
    GDALDataType m_eType;
    
    friend class EMURasterBand;
};

class EMURasterBand final: public GDALRasterBand
{
public:
    EMURasterBand(EMUDataset *, int nBandIn, GDALDataType eType, bool bThematic);
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
     
    bool GetThematic() { return m_bThematic;};
private:
    bool m_bNoDataSet;
    int64_t m_nNoData;
    bool m_bThematic;

    friend class EMUDataset;
};


EMUDataset::EMUDataset(VSILFILE *fp, GDALDataType eType, int nXSize, int nYSize, GDALAccess eInAccess)
{
    m_fp = fp;
    for( int i = 0; i < 6; i++ )
        m_padfTransform[i] = 0;
    m_padfTransform[1] = 1;
    m_padfTransform[5] = -1;
    
    m_eType = eType;
    nRasterXSize = nXSize;
    nRasterYSize = nYSize; 
    eAccess = eInAccess;
    
    // TODO: allow override
    m_tileSize = 512;
}

EMUDataset::~EMUDataset()
{
    Close();
}

CPLErr EMUDataset::Close()
{
    CPLErr eErr = CE_None;
    if( (nOpenFlags != OPEN_FLAGS_CLOSED ) && (eAccess == GA_Update) )
    {
        if( FlushCache(true) != CE_None )
            eErr = CE_Failure;

        if( m_fp )  
        {
            // now write header
            vsi_l_offset headerOffset = VSIFTellL(m_fp);
            VSIFWriteL("HDR", 4, 1, m_fp);
            
            // TODO: endianness
            uint64_t val = m_eType;
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            val = GetRasterCount();
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            val = GetRasterXSize();
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            val = GetRasterYSize();
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            // number of overviews
            val = 0;
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            // TODO: info on each overview here
            
            // nodata for each band. 
            for( int n = 0; n < GetRasterCount(); n++ )
            {
                EMURasterBand *pBand = cpl::down_cast<EMURasterBand*>(GetRasterBand(n + 1));
                int nNoDataSet;
                int64_t nodata = pBand->GetNoDataValueAsInt64(&nNoDataSet);
                // coerce so we know the size
                uint8_t n8NoDataSet = nNoDataSet;
                VSIFWriteL(&n8NoDataSet, sizeof(n8NoDataSet), 1, m_fp);
                VSIFWriteL(&nodata, sizeof(nodata), 1, m_fp);
                uint8_t nThematic = pBand->GetThematic();
                VSIFWriteL(&nThematic, sizeof(nThematic), 1, m_fp);
            }
            
            // tilesize
            VSIFWriteL(&m_tileSize, sizeof(m_tileSize), 1, m_fp);
            
            // geo transform
            VSIFWriteL(&m_padfTransform, sizeof(m_padfTransform), 1, m_fp);
            
            // projection
            char *pszWKT;
            m_oSRS.exportToWkt(&pszWKT);
            val = strlen(pszWKT) + 1;
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            VSIFWriteL(pszWKT, strlen(pszWKT) + 1, 1, m_fp);
            CPLFree(pszWKT);
            
            // number of tile offsets
            val = m_tileOffsets.size();
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            // now all the tile offsets
            for( const std::pair<const EMUTileKey, EMUTileValue>& n: m_tileOffsets)
            {
                VSIFWriteL(&n.second.offset, sizeof(n.second.offset), 1, m_fp);
                VSIFWriteL(&n.second.size, sizeof(n.second.size), 1, m_fp);
                VSIFWriteL(&n.first.ovrLevel, sizeof(n.first.ovrLevel), 1, m_fp);
                VSIFWriteL(&n.first.band, sizeof(n.first.band), 1, m_fp);
                VSIFWriteL(&n.first.x, sizeof(n.first.x), 1, m_fp);
                VSIFWriteL(&n.first.y, sizeof(n.first.y), 1, m_fp);
            }
            
            // now the offset of the start of the header
            VSIFWriteL(&headerOffset, sizeof(headerOffset), 1, m_fp);
                   
            VSIFCloseL(m_fp);
            m_fp = nullptr;
        }
    }
    return eErr;
}

void EMUDataset::setTileOffset(uint64_t o, uint64_t band, uint64_t x, 
    uint64_t y, vsi_l_offset offset, uint64_t size)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_tileOffsets.insert({{o, band, x, y}, {offset, size}});
}

EMUTileValue EMUDataset::getTileOffset(uint64_t o, uint64_t band, uint64_t x, uint64_t y)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_tileOffsets[{o, band, x, y}];
}


GDALDataset *EMUDataset::Open(GDALOpenInfo *poOpenInfo)
{
    if (!Identify(poOpenInfo))
        return nullptr;
        
     // Confirm the requested access is supported.
    if( poOpenInfo->eAccess == GA_Update )
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "The EMU driver does not support update access to existing "
                 "datasets.");
        return nullptr;
    }

    if( (poOpenInfo->pabyHeader[0] != 'E') && 
        (poOpenInfo->pabyHeader[1] != 'M') &&
        (poOpenInfo->pabyHeader[2] != 'U') )
    {
        return nullptr;
    }
    
    // TODO: get version    
    
    // Check that the file pointer from GDALOpenInfo* is available.
    if( poOpenInfo->fpL == nullptr )
    {
        return nullptr;
    }
    

    // seek to the end of the file
    VSIFSeekL(poOpenInfo->fpL, 0 , SEEK_END);
    vsi_l_offset fsize = VSIFTellL(poOpenInfo->fpL);
    
    // seek to the size of the header offset
    // hopefully these accesses are cached?
    uint64_t headerOffset; 
    VSIFSeekL(poOpenInfo->fpL, fsize - sizeof(headerOffset), SEEK_SET);
    VSIFReadL(&headerOffset, sizeof(headerOffset), 1, poOpenInfo->fpL);
    VSIFSeekL(poOpenInfo->fpL, headerOffset, SEEK_SET);
    
    char headerChars[4];
    VSIFReadL(headerChars, 4, 1, poOpenInfo->fpL);
    if( strcmp(headerChars, "HDR") != 0 )
    {
         CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to read header");
        return nullptr;       
    }
    
    uint64_t ftype;
    VSIFReadL(&ftype, sizeof(ftype), 1, poOpenInfo->fpL);
    
    uint64_t count;
    VSIFReadL(&count, sizeof(count), 1, poOpenInfo->fpL);
    
    uint64_t xsize;
    VSIFReadL(&xsize, sizeof(xsize), 1, poOpenInfo->fpL);

    uint64_t ysize;
    VSIFReadL(&ysize, sizeof(ysize), 1, poOpenInfo->fpL);

    uint64_t nover;
    VSIFReadL(&nover, sizeof(nover), 1, poOpenInfo->fpL);
    
    // grap ownership of poOpenInfo->fpL
    VSILFILE *fp = nullptr;
    std::swap(fp, poOpenInfo->fpL);

    GDALDataType eType = (GDALDataType)ftype;
    EMUDataset *pDS = new EMUDataset(fp, eType, xsize, ysize, GA_ReadOnly);

    // nodata for each band. 
    for( int n = 0; n < count; n++ )
    {
        uint8_t n8NoDataSet;
        VSIFReadL(&n8NoDataSet, sizeof(n8NoDataSet), 1, fp);
        int64_t nodata;
        VSIFReadL(&nodata, sizeof(nodata), 1, fp);
        uint8_t nThematic;
        VSIFReadL(&nThematic, sizeof(nThematic), 1, fp);

        EMURasterBand *pBand = new EMURasterBand(pDS, n + 1, eType, nThematic);
        if(n8NoDataSet)
            pBand->SetNoDataValueAsInt64(nodata);
        
        pDS->SetBand(n + 1, pBand);
    }
    
    uint64_t tilesize;
    VSIFReadL(&tilesize, sizeof(tilesize), 1, fp);
    
    double transform[6];
    VSIFReadL(transform, sizeof(transform), 1, fp);
    pDS->SetGeoTransform(transform);
    
    uint64_t wktSize;
    VSIFReadL(&wktSize, sizeof(wktSize), 1, fp);
    
    char *pszWKT = new char[wktSize];
    VSIFReadL(pszWKT, wktSize, 1, fp);
    OGRSpatialReference sr(pszWKT);
    pDS->SetSpatialRef(&sr);
    delete[] pszWKT;
    
    uint64_t ntiles;
    VSIFReadL(&wktSize, sizeof(wktSize), 1, fp);
    
    for( uint64_t n = 0; n < ntiles; n++ )
    {
        uint64_t offset;
        VSIFReadL(&offset, sizeof(offset), 1, fp);
        uint64_t size;
        VSIFReadL(&size, sizeof(size), 1, fp);
        uint64_t ovrLevel;
        VSIFReadL(&ovrLevel, sizeof(ovrLevel), 1, fp);
        uint64_t band;
        VSIFReadL(&band, sizeof(band), 1, fp);
        uint64_t x;
        VSIFReadL(&x, sizeof(x), 1, fp);
        uint64_t y;
        VSIFReadL(&y, sizeof(y), 1, fp);
        
        pDS->setTileOffset(ovrLevel, band, x, y, offset, size);
    }

    return pDS;
}

int EMUDataset::Identify(GDALOpenInfo *poOpenInfo)
{
    if( !EQUAL(CPLGetExtension(poOpenInfo->pszFilename), "EMU") )
        return FALSE;
        
    if (poOpenInfo->pabyHeader == nullptr ||
        memcmp(poOpenInfo->pabyHeader, "EMU", 3) != 0)
    {
        return FALSE;
    }
    
    return TRUE;
}

GDALDataset *EMUDataset::Create(const char * pszFilename,
                                int nXSize, int nYSize, int nBands,
                                GDALDataType eType,
                                char ** /* papszParamList */)
{
    if( (eType < GDT_Byte ) || (eType > GDT_Int16) )
    {
        // for now, I'm worried about building the histogram otherwise
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Attempt to create EMU labeled dataset with an illegal "
            "data type (%s).",
            GDALGetDataTypeName(eType));
        return NULL;
    }

    // Try to create the file.
    VSILFILE *fp = VSIFOpenL(pszFilename, "w");
    if( fp == NULL )
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                "Attempt to create file `%s' failed.",
                pszFilename);
        return NULL;
    }
    
    VSIFPrintfL(fp, "EMU%04d", EMU_VERSION);
    
    EMUDataset *pDS = new EMUDataset(fp, eType, nXSize, nYSize, GA_Update);
    for( int n = 0; n < nBands; n++ )
    {
        pDS->SetBand(n + 1, new EMURasterBand(pDS, n + 1, eType, false));
    }
    
    return pDS;
}


CPLErr EMUDataset::GetGeoTransform( double * padfTransform )
{
    for( int i = 0; i < 6; i++ )
    {
        padfTransform[i] = m_padfTransform[i];
    }
    
    return CE_None;
}

CPLErr EMUDataset::SetGeoTransform( double * padfTransform )
{
    for( int i = 0; i < 6; i++ )
    {
        m_padfTransform[i] = padfTransform[i];
    }

    return CE_None;
}

const OGRSpatialReference* EMUDataset::GetSpatialRef() const
{
    return &m_oSRS;
}

CPLErr EMUDataset::SetSpatialRef(const OGRSpatialReference* poSRS)
{
    m_oSRS = *poSRS;
    return CE_None;
}

EMURasterBand::EMURasterBand(EMUDataset *pDataset, int nBandIn, GDALDataType eType, bool bThematic)
{
    poDS = pDataset;
    nBlockXSize = 512;
    nBlockYSize = 512;
    nBand = nBandIn;
    eDataType = eType;
    m_bNoDataSet = false;
    m_nNoData = 0;
    m_bThematic = bThematic;

    nRasterXSize = pDataset->GetRasterXSize();          // ask the dataset for the total image size
    nRasterYSize = pDataset->GetRasterYSize();
}

EMURasterBand::~EMURasterBand()
{
    
}

CPLErr EMURasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pData)
{
    EMUDataset *poEMUDS = cpl::down_cast<EMUDataset *>(poDS);

    EMUTileValue val;
    try
    {
        val = poEMUDS->getTileOffset(0, nBand, nBlockXOff, nBlockYOff);
    }
    catch(const std::out_of_range& oor)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                "Couldn't find index for block %d %d.",
                nBlockXOff, nBlockXOff);
        return CE_Failure;
    }
    
    fprintf(stderr, "wqwddqw %d %d\n", (int)val.offset, (int)val.size);
    VSIFSeekL(poEMUDS->m_fp, val.offset, SEEK_SET);
    VSIFReadL(pData, val.size, 1, poEMUDS->m_fp);

    return CE_None;
}

CPLErr EMURasterBand::IWriteBlock(int nBlockXOff, int nBlockYOff, void *pData)
{
    EMUDataset *poEMUDS = cpl::down_cast<EMUDataset *>(poDS);
    
    // GDAL deals in blocks - if we are at the end of a row
    // we need to adjust the amount read so we don't go over the edge
    int nxsize = this->nBlockXSize;
    int nxtotalsize = this->nBlockXSize * (nBlockXOff + 1);
    if( nxtotalsize > this->nRasterXSize )
    {
        nxsize -= (nxtotalsize - this->nRasterXSize);
    }
    int nysize = this->nBlockYSize;
    int nytotalsize = this->nBlockYSize * (nBlockYOff + 1);
    if( nytotalsize > this->nRasterYSize )
    {
        nysize -= (nytotalsize - this->nRasterYSize);
    }
    
    vsi_l_offset tileOffset = VSIFTellL(poEMUDS->m_fp);
    int typeSize = GDALGetDataTypeSize(eDataType) / 8;
    
    // size of block
    uint64_t blockSize = (nxsize * typeSize) * (nysize * typeSize);
    
    // TODO: compression
    uint64_t compression = COMPRESSION_NONE;
    VSIFWriteL(&compression, sizeof(compression), 1, poEMUDS->m_fp);

    // data
    VSIFWriteL(pData, blockSize, 1, poEMUDS->m_fp);
    
    // update map
    poEMUDS->setTileOffset(0, nBand, nBlockXOff, nBlockYOff, tileOffset, blockSize);

    // TODO: read data to work out stats and pyramid layers 
    
    return CE_None;
}

double EMURasterBand::GetNoDataValue(int *pbSuccess/* = nullptr*/)
{
    if( pbSuccess )
        *pbSuccess = m_bNoDataSet;
    return m_nNoData;
}

int64_t EMURasterBand::GetNoDataValueAsInt64(int *pbSuccess/* = nullptr*/)
{
    if( pbSuccess )
        *pbSuccess = m_bNoDataSet;
    return m_nNoData;
}

uint64_t EMURasterBand::GetNoDataValueAsUInt64(int *pbSuccess/* = nullptr*/)
{
    if( pbSuccess )
        *pbSuccess = m_bNoDataSet;
    return m_nNoData;
}

CPLErr EMURasterBand::SetNoDataValue(double dfNoData)
{
    m_nNoData = dfNoData;
    m_bNoDataSet = true;
    return CE_None;
}

CPLErr EMURasterBand::SetNoDataValueAsInt64(int64_t nNoData)
{
    m_nNoData = nNoData;
    m_bNoDataSet = true;
    return CE_None;
}

CPLErr EMURasterBand::SetNoDataValueAsUInt64(uint64_t nNoData)
{
    m_nNoData = nNoData;
    m_bNoDataSet = true;
    return CE_None;
}

CPLErr EMURasterBand::DeleteNoDataValue()
{
    m_bNoDataSet = false;   
    return CE_None;
}


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

    poDriver->pfnOpen = EMUDataset::Open;
    poDriver->pfnIdentify = EMUDataset::Identify;
    poDriver->pfnCreate = EMUDataset::Create;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
