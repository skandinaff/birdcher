# IMX415 Device Tree facts and constraints

No VIM3 IMX415 overlay has been created yet. The required VIM3-specific
wiring facts have not been established, and guessing them could hold the
sensor in reset, drive the wrong rail, or claim another I²C device.

## Datasheet-derived requirements

Source: `docs/datasheets/imx415-datasheet.pdf`, Sony IMX415-AAQR-C, printed
pages 1, 3, 14–19, 26, 74, 78–80, and 84.

- Supplies: DVDD = 1.1 V (1.00–1.20 V), OVDD = 1.8 V (1.70–1.90 V), AVDD =
  2.9 V (2.80–3.00 V).
- Bring rails up in the order DVDD → OVDD → AVDD and finish all rises within
  200 ms. Afterward hold XCLR low for at least 500 ns, deassert XCLR, start
  INCK at least 1 µs later, and wait at least 20 µs before I²C communication.
- Valid INCK frequencies are 24, 27, 37.125, 72, and 74.25 MHz. A 24 MHz
  input is valid with the datasheet's 720 and 1440 Mbps-per-lane settings.
- CSI-2 supports 2 or 4 data lanes and RAW10 or RAW12. `LANEMODE=1` selects
  2 lanes; `LANEMODE=3` selects 4 lanes.
- I²C uses a 16-bit register address, 8-bit data, and up to 400 kHz.
- The 7-bit I²C address depends on SLAMODE straps: `0x1a`, `0x10`, `0x36`, or
  `0x37` for (SLAMODE1, SLAMODE0) of (low,low), (low,high), (high,low), and
  (high,high), respectively. A scan alone does not identify the strap state.

## VIM3 V15 schematic mapping (revision-provisional)

Khadas's VIM3 V15 schematic (dated 2024-04-07) identifies J12 as the
30-pin Camera0 connector. This must be confirmed against the physical board
revision before it is used for hardware control.

- J12 exposes a four-lane CSI-2 bus: D3 on pins 2/3, D2 on 5/6, clock B on
  8/9, D1 on 11/12, D0 on 14/15, and clock A on 17/18. The IMX415 must use
  four data lanes only if its camera module connects all four lanes.
- J12 pins 22/23 are I2C_AO_SCL/I2C_AO_SDA. The running system maps this
  controller to i2c-0, /soc/bus@ff800000/i2c@5000; it already contains
  the board's RTC, MCU, and TCA6408 GPIO expander.
- The TCA6408 is live at I2C address 0x20 as gpio_expander (Linux
  gpiochip612). V15 connects expander outputs P2, P3, and P4 to
  CAM_RESET, CAM_PDN0, and CAM_PDN1, respectively. This establishes
  line numbers, not the connected camera module's required polarity or which
  PDN signal it uses; none will be toggled until that is verified.
- J12 pin 20 (CAM_MCLK) is routed to GPIOAO_10, whose documented alternate
  function is CLK12_24; this is a viable 24 MHz INCK source. Its pinmux and
  clock provider must be enabled in the eventual overlay.
- J12 supplies VDDIO_AO18 on pin 25 and 3.3 V on pins 27/28. It does not
  itself prove that the IMX415's required 1.1 V, 1.8 V, and 2.9 V rails are
  present or sequenced. That remains a property of the attached camera board.

The same schematic shows the host receiver pins as NC(CSI_*), consistent
with the absence of a mainline G12B CSI capture driver. The connector and
sensor can therefore be described accurately, but a functional capture graph
also requires a ported/maintained receiver, PHY, and ISP path.

## Values still required from the VIM3

- the actual strap-selected sensor address (one of the datasheet's four);
- confirmation that the physical board is V15-equivalent and that the
  attached module's FFC orientation/lane map matches J12;
- the connected lane count and lane ordering on the camera module;
- the camera module's use and polarity of CAM_RESET, CAM_PDN0, and CAM_PDN1;
- confirmation that CLK12_24 is present at the module and acceptable to its
  oscillator/input circuit;
- availability and sequencing control of 1.1 V, 1.8 V, and 2.9 V rails;

Only after those facts are verified may an upstream-style `sony,imx415` node
be written with `clocks`, `reset-gpios`, three supply phandles, and a standard
V4L2 endpoint graph.
