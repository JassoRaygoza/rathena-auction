# Deterministic integer-X2 asset build: preserve the original palette and every
# source pixel (including the magenta colour key). No filtering or new artwork.
param([Parameter(Mandatory=$true)][string]$Textures,[Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class AuctionBitmapX2 {
    static void Put(byte[] data,int offset,int value) {Array.Copy(BitConverter.GetBytes(value),0,data,offset,4);}
    public static int[] Expand(string source,string target) {
        byte[] data=File.ReadAllBytes(source);
        if(data.Length<54 || data[0]!=66 || data[1]!=77)throw new Exception("Not a BMP");
        int offset=BitConverter.ToInt32(data,10),dib=BitConverter.ToInt32(data,14);
        int width=BitConverter.ToInt32(data,18),signedHeight=BitConverter.ToInt32(data,22);
        int height=Math.Abs(signedHeight),bpp=BitConverter.ToUInt16(data,28);
        if(dib<40 || width<1 || height<1 || width>4096 || height>4096 ||
           (bpp!=8 && bpp!=24 && bpp!=32) || BitConverter.ToInt32(data,30)!=0 ||
           BitConverter.ToUInt16(data,26)!=1 || offset<54)throw new Exception("Unsupported BMP format");
        int bytes=bpp/8,stride=(width*bytes+3)&~3,newStride=(width*2*bytes+3)&~3;
        if(offset+(long)stride*height>data.Length)throw new Exception("Truncated pixels");
        byte[] output=new byte[offset+newStride*height*2];
        Array.Copy(data,output,offset);
        Put(output,2,output.Length);Put(output,18,width*2);Put(output,22,signedHeight*2);Put(output,34,newStride*height*2);
        for(int y=0;y<height;y++)for(int x=0;x<width;x++)for(int dy=0;dy<2;dy++)for(int dx=0;dx<2;dx++)
            Array.Copy(data,offset+y*stride+x*bytes,output,offset+(y*2+dy)*newStride+(x*2+dx)*bytes,bytes);
        // Verify all output pixels, not just dimensions or a few samples.
        for(int y=0;y<height*2;y++)for(int x=0;x<width*2;x++)for(int c=0;c<bytes;c++)
            if(output[offset+y*newStride+x*bytes+c]!=data[offset+(y/2)*stride+(x/2)*bytes+c])
                throw new Exception("Pixel expansion mismatch");
        using(var file=new FileStream(target,FileMode.CreateNew))file.Write(output,0,output.Length);
        return new int[]{width,height,width*2,height*2,bpp};
    }
}
'@
$auctionSource = Get-ChildItem -LiteralPath $Textures -Recurse -File -Filter auction_bg_0.bmp
if (@($auctionSource).Count -ne 1) { throw 'Expected exactly one original Auction skin directory' }
$auctionSkinSource = $auctionSource.Directory.FullName
$auctionUiName = $auctionSource.Directory.Parent.Name
$auctionTarget = Join-Path $Output ('data\texture\' + $auctionUiName + '\basic_interface\auction_x2')
New-Item -ItemType Directory -Path $auctionTarget -Force | Out-Null
$auctionEntries = foreach ($auctionBmp in (Get-ChildItem -LiteralPath $auctionSkinSource -File -Filter 'auction_*.bmp')) {
    $auctionNewBmp = Join-Path $auctionTarget $auctionBmp.Name
    $auctionDimensions = [AuctionBitmapX2]::Expand($auctionBmp.FullName,$auctionNewBmp)
    @{ name=$auctionBmp.Name; source=$auctionBmp.FullName; destination=$auctionNewBmp;
       source_sha256=(Get-FileHash -LiteralPath $auctionBmp.FullName).Hash.ToLower();
       sha256=(Get-FileHash -LiteralPath $auctionNewBmp).Hash.ToLower();
       width=$auctionDimensions[2];height=$auctionDimensions[3];bpp=$auctionDimensions[4];
       source_width=$auctionDimensions[0];source_height=$auctionDimensions[1];all_pixels_verified=$true }
}
if (@($auctionEntries).Count -ne 15) { throw 'Expected all 15 original Auction BMPs' }
@{ method='exact integer 2x pixel replication; original palette and colour key preserved'; scale=2;files=$auctionEntries } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Output 'skin_manifest.json') -Encoding UTF8
Write-Output '15 BMPs expanded and verified pixel by pixel.'
