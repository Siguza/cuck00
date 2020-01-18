# cuck00

XNU/IOKit info leak 1day.  
Killed in iOS 13.3.1 beta 2.

Write-up [here](https://siguza.github.io/cuck00/).

### Building

For macOS:

    make

For iOS:

    CC='xcrun -sdk iphoneos clang -arch arm64' make
