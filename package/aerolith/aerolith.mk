################################################################################
#
# aerolith
#
################################################################################

# Pin this to a tag or commit hash once one exists, for reproducible builds.
# Overridable from the defconfig/environment, e.g. BR2_PACKAGE_AEROLITH_OVERRIDE_SRCDIR
# via Buildroot's package override mechanism for local development.
AEROLITH_VERSION = main
AEROLITH_SITE = $(call github,FernandesKA,aerolith,$(AEROLITH_VERSION))

define AEROLITH_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(@D)/buildroot/S99aerolith \
		$(TARGET_DIR)/etc/init.d/S99aerolith
endef

$(eval $(cmake-package))
