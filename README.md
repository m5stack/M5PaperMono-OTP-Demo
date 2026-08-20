# PaperMono OTP LUT Demo

### SKU: C153, C153-LITE

## Related Link

- [C153 Document & Datasheet](https://docs.m5stack.com/en/products/sku/C153).
- [C153-LITE Document & Datasheet](https://docs.m5stack.com/en/products/sku/C153-LITE).

This project is a pure ESP-IDF demo for M5Stack PaperMono devices. It drives the
SSD1677 controller on the DEPG0397BBS770F3HP-XM display module directly over SPI and
uses the controller's built-in OTP waveforms to refresh the display.

## Demo Features

- Partial refresh: a black quadrant moves from upper-left to upper-right, lower-left,
  and lower-right after each screen touch.
- Monochrome full refresh: displays two black and two white quadrants.
- Four-gray full refresh: displays white, light gray, dark gray, and black quadrants.
- Deep sleep after every refresh.
- Touch-driven demo sequence with no automatic playback.

After the four-gray example, the next touch restores a monochrome baseline and starts
the sequence again from the upper-left partial-refresh pattern.

## IDF Build

### Toolchain

[ESP-IDF v5.5.1](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/index.html)

### Configure Target

```bash
idf.py set-target esp32s3
```

### Build

```bash
idf.py build
```

### Flash

```bash
idf.py -p PORT flash
```

## Acknowledgments

This project uses the following open-source libraries and resources:

- https://github.com/m5stack/M5Unified
- https://github.com/m5stack/M5GFX

## License

- [MIT](LICENSE)
