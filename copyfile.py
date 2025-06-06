#!/usr/bin/env python3
#
#  copyfile.py
#  EMUFormat
#
#  Created by Sam Gillingham on 26/03/2024.
#  Copyright 2024 EMUFormat. All rights reserved.
#
#  This file is part of EMUFormat.
#
#  Permission is hereby granted, free of charge, to any person 
#  obtaining a copy of this software and associated documentation 
#  files (the "Software"), to deal in the Software without restriction, 
#  including without limitation the rights to use, copy, modify, 
#  merge, publish, distribute, sublicense, and/or sell copies of the 
#  Software, and to permit persons to whom the Software is furnished 
#  to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be 
#  included in all copies or substantial portions of the Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
#  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES 
#  OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
#  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR 
#  ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF 
#  CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION 
#  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#

import sys
from rios import applier, fileinfo

from osgeo import gdal
gdal.UseExceptions()


def copyf(info, inputs, outputs):
    outputs.outf = inputs.inf

def main():
    inputs = applier.FilenameAssociations()
    inputs.inf = sys.argv[1]
    
    outputs = applier.FilenameAssociations()
    outputs.outf = sys.argv[2]
    
    # is input thematic?
    info = fileinfo.ImageInfo(sys.argv[1], omitPerBand=True)
    thematic = info.layerType == 'thematic'
    
    controls = applier.ApplierControls()
    controls.setOutputDriverName('EMU')
    controls.setWindowSize(512, 512)
    controls.setSinglePassPyramids(True)
    controls.setThematic(thematic)
    
    applier.apply(copyf, inputs, outputs, controls=controls)

if __name__ == '__main__':
    main()
