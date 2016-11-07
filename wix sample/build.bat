pushd %~dp0
del main.wix*
del main.msi
pause
D:\Perforce\NW\main\external\wix\candle.exe main.wxs
D:\Perforce\NW\main\external\wix\light.exe main.wixobj
pupd