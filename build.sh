    #!/bin/bash

    set -e  # Exit on any error

    echo "Building MIDI Clock Sync..."

    # Compiler flags
    CXX="g++"
    CXXFLAGS="-std=c++17 -O3 -Wall -Wextra"
    LDFLAGS="-lasound -ljack -lpthread -latomic"

    # Source and output
    SOURCE="midi_clock_sync.cpp"
    OUTPUT="midi_clock_sync"

    # Compile
    $CXX $CXXFLAGS $SOURCE -o $OUTPUT $LDFLAGS

    if [ $? -eq 0 ]; then
        echo "â Build complete: $OUTPUT"
        echo ""
        echo "Run with:"
        echo "  pw-jack ./$OUTPUT 32:0"
        echo ""
        echo "Or install system-wide:"
        echo "  sudo cp $OUTPUT /usr/local/bin/"
        echo "  sudo setcap cap_sys_nice+ep /usr/local/bin/$OUTPUT"
    else
        echo "â Build failed!"
        exit 1
    fi
