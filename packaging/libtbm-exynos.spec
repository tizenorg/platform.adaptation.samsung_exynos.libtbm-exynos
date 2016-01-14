Name:           libtbm-exynos
Version:        1.2.1
Release:        1
License:        MIT
Summary:        Tizen Buffer Manager - exynos backend
Group:          System/Libraries
ExcludeArch:    i586
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  pkgconfig(pthread-stubs)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libdrm_exynos)
BuildRequires:  pkgconfig(libtbm)
BuildRequires:  pkgconfig(dlog)
BuildRequires:  pkgconfig(libudev)

%description
descriptionion: Tizen Buffer manager backend module for exynos

%prep
%setup -q

%build

%reconfigure --prefix=%{_prefix} --libdir=%{_libdir}/bufmgr --disable-cachectrl \
            CFLAGS="${CFLAGS} -Wall -Werror" LDFLAGS="${LDFLAGS} -Wl,--hash-style=both -Wl,--as-needed"

make %{?_smp_mflags}

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}/usr/share/license
cp -af COPYING %{buildroot}/usr/share/license/%{name}
%make_install


%post
if [ -f %{_libdir}/bufmgr/libtbm_default.so ]; then
    rm -rf %{_libdir}/bufmgr/libtbm_default.so
fi
ln -s libtbm_exynos.so %{_libdir}/bufmgr/libtbm_default.so

%postun -p /sbin/ldconfig

%files
%{_libdir}/bufmgr/libtbm_*.so*
/usr/share/license/%{name}

