Name:           libtbm-dumb
Version:        1.0.9
Release:        1
License:        MIT
Summary:        Tizen Buffer Manager - drm dumb backend
Group:          System/Libraries
ExcludeArch:    i586
Source0:        %{name}-%{version}.tar.gz
Source1001:	%name.manifest

BuildRequires:  pkgconfig(pthread-stubs)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libtbm)
BuildRequires:  pkgconfig(dlog)

%description
descriptionion: Tizen Buffer manager backend module uses drm dumb

%prep
%setup -q
cp %{SOURCE1001} .
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
ln -s libtbm_dumb.so %{_libdir}/bufmgr/libtbm_default.so

%postun -p /sbin/ldconfig

%files
%manifest %{name}.manifest
%{_libdir}/bufmgr/libtbm_*.so*
/usr/share/license/%{name}

