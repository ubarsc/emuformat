# EMU Format

## What is this?

An experimental format that can be streamed to S3 (and read from s3 efficiently). 
It only supports a subset of GDAL features. It is envisaged this will only be of use
to people using AWS Batch who are sick of having to expand the local storage
using a LaunchTemplate to accomodate other formats that must be written 
locally before copying to S3. Files in EMU format can be written directly to S3
using the /vsis3 GDAL virtual filesystem.

It is likely too, that EMU will be used as an intermediate format before 
being translated into GeoTiff or KEA for distribution. 


It is designed to be written by:

1. RIOS, in particular version 2.0.5 and above
with the on-the-fly statistics and pyramid layers enabled (the default). 

2. gdal_translate -of EMU from another GDAL supported format. If available,
the statistics and pyramid layers will be copied across from the input file.

Other methods may result in errors or corruption of data. 

It doesn't (and will not) support update of files in place. 

## What is this not?

A general format for imagery. See the KEA format for a better alternative.

## Implemented Features

- Overviews
- Statistics
- Metadata
- Raster Attribute Tables (but implementation incomplete)
- Projections

## FAQ's

Q. Does it work under Windows?
A. Dunno

Q. Are you going to add support for other features not already covered above?
A. No

Q. How to build?
A. On Linux conda:
```
conda create -n emu cmake cxx-compiler gdal
conda activate emu
mkdir build
cd build
cmake -D CMAKE_INSTALL_PREFIX=$CONDA_PREFIX ..
make
make install
```
On Ubuntu, the recipe is similar. Ensure you have `libgdal-dev` and `g++` packages installed.
Set the `GDAL_DRIVER_PATH` env var to the location of the installed library.
