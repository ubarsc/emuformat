# EMU Format

## What is this?

An experimental format that can be streamed to S3 (and read from s3 efficiently). 
It only supports a subset of GDAL features. It is envisaged this will only be of use
to people using AWS Batch who are sick of having to expand the local storage
using a LaunchTemplate to accomodate other formats that must be written 
locally before copying to S3. Files in EMU format can be written directly to S3
using the /vsis3 GDAL virtual filesystem.

It is likely too, that EMU will be used as an intermediate formate before 
being translated into GeoTiff or KEA for distribution. 

It will support overviews (calculated as it goes) and does support
the calculation of stats as it goes. It will soon support histograms
and basic Raster Attribute Tables.

It is designed to be written only by RIOS. Other methods may result
in errors or corruption of data. 

Furthermore with RIOS, the blocksize must be set to 512x512 and
the calculation of stats/pyramid layers must be disabled. See
the file copyfile.py for more information.

It doesn't (and will not) support update of files in place. 

## What is this not?

A general format for imagery. See the KEA format for a better alternative.

## TODOs

- Histograms
- RAT
- Calculation of pyramid layers on writing.

## FAQ's

Q. Should I use this operationally?
A. Absolutely not

Q. Does it work under Windows?
A. Dunno

Q. Are you going to add support for other features not already covered above?
A. No


