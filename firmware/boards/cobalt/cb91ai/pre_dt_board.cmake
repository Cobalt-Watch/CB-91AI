# SPDX-License-Identifier: Apache-2.0

# SPI is implemented via shim on nRF52 SoCs: suppress the DTC warning about
# the SPI node being used as a bus bridge (same as the Nordic DK boards).
list(APPEND EXTRA_DTC_FLAGS "-Wno-spi_bus_bridge")
