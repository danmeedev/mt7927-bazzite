# SPDX-License-Identifier: GPL-2.0
#
# mt7927-kmod.spec - Akmod package for MT7927 WiFi driver
#
# For Bazzite and other Fedora Atomic/rpm-ostree systems
#

%global debug_package %{nil}

# Fedora kmod macros
%if 0%{?fedora}
%global buildforkernels akmod
%endif

Name:           mt7927-kmod
Version:        0.1.0
Release:        1%{?dist}
Summary:        Kernel module for MediaTek MT7927 WiFi 7 (AMD RZ738)
License:        GPL-2.0-only
URL:            https://github.com/mt7927-linux/mt7927-driver

Source0:        mt7927-%{version}.tar.gz

BuildRequires:  kmodtool
BuildRequires:  elfutils-libelf-devel
BuildRequires:  kernel-devel

# Required for Bazzite/Universal Blue
Provides:       mt7927-kmod = %{version}-%{release}
Provides:       kmod(mt7927.ko) = %{version}

%description
This package contains the kernel module for MediaTek MT7927 WiFi 7
wireless network adapter, also known as AMD RZ738 on AMD platforms.

The MT7927 is a WiFi 7 (802.11be) chip supporting up to 320MHz channels
on the 6GHz band. This driver is based on the mt7925 driver from the
Linux kernel mt76 framework.

%package -n akmod-mt7927
Summary:        Akmod package for MT7927 WiFi driver
BuildRequires:  kmodtool
Requires:       akmods
Requires:       mt7927-firmware >= 1.0

%description -n akmod-mt7927
This package provides the akmod package for the MT7927 WiFi driver.
It will automatically rebuild the kernel module when the kernel is updated.

%prep
%setup -q -n mt7927-%{version}

%build
# The actual build happens via akmods at install time
# This section prepares the source for akmods

%install
# Install source for akmods
mkdir -p %{buildroot}%{_usrsrc}/akmods/mt7927-%{version}
cp -a driver/* %{buildroot}%{_usrsrc}/akmods/mt7927-%{version}/

# Create akmods config
mkdir -p %{buildroot}%{_sysconfdir}/akmods
cat > %{buildroot}%{_sysconfdir}/akmods/mt7927.conf << 'EOF'
# MT7927 akmods configuration
PACKAGE_NAME=mt7927
PACKAGE_VERSION=%{version}
BUILT_MODULE_NAME[0]=mt7927
DEST_MODULE_LOCATION[0]=/kernel/drivers/net/wireless/mediatek/mt76/mt7927
AUTOINSTALL=yes
EOF

%files -n akmod-mt7927
%license LICENSE
%doc README.md
%{_usrsrc}/akmods/mt7927-%{version}/
%config(noreplace) %{_sysconfdir}/akmods/mt7927.conf

%changelog
* Sun Feb 02 2026 MT7927 Project <mt7927@example.com> - 0.1.0-1
- Initial package
- Implements correct power management handoff sequence
- Implements WFSYS reset to unlock registers
- Implements DMA initialization sequence
- Target: AMD systems with MT7927/RZ738
