<#
.SYNOPSIS
    生成一个 32x32 32bpp 有效 .ico 图标（蓝色渐变圆形 + 十字线节点）
#>
param(
    [Parameter(Mandatory=$true)]
    [string]$OutFile
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Collections.Generic;

public static class IconGen {
    public static void Generate(string outFile) {
        int W = 32, H = 32, bpp = 32;
        var data = new List<byte>();

        // ICO Header (6 bytes): reserved=0, type=1, count=1
        data.AddRange(new byte[]{0,0,1,0,1,0});

        // ICO Entry (16 bytes)
        int imgDataSize = 40 + W * H * 4;  // BITMAPINFOHEADER + pixels
        int imgOffset = 22;

        data.AddRange(new byte[]{ (byte)W, (byte)H, 0, 0, 1, 0, (byte)bpp, 0 });
        data.AddRange(BitConverter.GetBytes(imgDataSize));
        data.AddRange(BitConverter.GetBytes(imgOffset));

        // BITMAPINFOHEADER (40 bytes)
        data.AddRange(BitConverter.GetBytes(40));          // biSize
        data.AddRange(BitConverter.GetBytes(W));            // biWidth
        data.AddRange(BitConverter.GetBytes(H * 2));        // biHeight
        data.AddRange(BitConverter.GetBytes((short)1));     // biPlanes
        data.AddRange(BitConverter.GetBytes((short)bpp));   // biBitCount
        data.AddRange(new byte[24]);                        // rest (zeros)

        // Pixel data: BGRA, bottom-up
        var pixels = new byte[W * H * 4];
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int idx = (y * W + x) * 4;
                double dx = x - 15.5;
                double dy = y - 15.5;
                double dist = Math.Sqrt(dx * dx + dy * dy);

                if (dist > 14) {
                    // transparent
                    pixels[idx]=0; pixels[idx+1]=0; pixels[idx+2]=0; pixels[idx+3]=0;
                } else if (dist > 13) {
                    int alpha = (int)Math.Round((14 - dist) * 255);
                    pixels[idx]=200; pixels[idx+1]=80; pixels[idx+2]=20; pixels[idx+3]=(byte)alpha;
                } else {
                    int bright = (int)(180 + (1 - dist / 13) * 75);
                    if (bright > 255) bright = 255;
                    pixels[idx]=(byte)bright; pixels[idx+1]=(byte)(bright*0.45); pixels[idx+2]=(byte)(bright*0.15); pixels[idx+3]=255;

                    double cx = Math.Abs(x - 15.5);
                    double cy = Math.Abs(y - 15.5);
                    if ((cy < 1.2 && cx < 7) || (cx < 1.2 && cy < 7)) {
                        pixels[idx]=255; pixels[idx+1]=200; pixels[idx+2]=100; pixels[idx+3]=255;
                    }
                }
            }
        }

        // Write pixels bottom-up for BMP
        for (int y = H - 1; y >= 0; y--) {
            int rowStart = y * W * 4;
            for (int i = 0; i < W * 4; i++) {
                data.Add(pixels[rowStart + i]);
            }
        }

        File.WriteAllBytes(outFile, data.ToArray());
    }
}
"@ -ReferencedAssemblies "System"

# 确保目标目录存在
$dir = Split-Path $OutFile -Parent
if ($dir -and -not (Test-Path $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

[IconGen]::Generate($OutFile)

$size = [math]::Round((Get-Item $OutFile).Length / 1024, 1)
Write-Host "Icon generated: $OutFile ($size KB)"
