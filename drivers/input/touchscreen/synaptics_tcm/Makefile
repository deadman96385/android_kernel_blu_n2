#
# Makefile for the Synaptics TCM touchscreen driver.
#

# Each configuration option enables a list of files.

obj-$(CONFIG_TOUCHSCREEN_SYNAPTICS_TCM_SPI) += synaptics_tcm_spi.o
obj-$(CONFIG_TOUCHSCREEN_SYNAPTICS_TCM_I2C) += synaptics_tcm_i2c.o
obj-$(CONFIG_TOUCHSCREEN_SYNAPTICS_TCM) += synaptics_tcm_core_module.o
synaptics_tcm_core_module-objs := synaptics_tcm_core.o synaptics_tcm_touch.o
obj-$(CONFIG_TOUCHSCREEN_SYNAPTICS_TCM) += synaptics_tcm_zeroflash.o
obj-$(CONFIG_TOUCHSCREEN_SYNAPTICS_TCM) += synaptics_tcm_reflash.o
obj-$(CONFIG_TOUCHSCREEN_SYNAPTICS_TCM) += synaptics_tcm_device.o
obj-$(CONFIG_TOUCHSCREEN_SYNAPTICS_TCM) += synaptics_tcm_testing.o
obj-$(CONFIG_TOUCHSCREEN_SYNAPTICS_TCM) += synaptics_tcm_recovery.o
obj-$(CONFIG_TOUCHSCREEN_SYNAPTICS_TCM) += synaptics_tcm_diagnostics.o
