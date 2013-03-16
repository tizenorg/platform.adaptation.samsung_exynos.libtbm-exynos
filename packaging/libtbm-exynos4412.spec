Name:           libtbm-exynos4412
Version:        1.0.2
Release:        2
License:        MIT
Summary:        Tizen Buffer Manager - exynos4412 backend
Group:          System/Libraries
ExcludeArch:    i586
Source0:        %{name}-%{version}.tar.gz

%description
descriptionion: ${summary}

%prep
%setup -q

%build

autoreconf -vfi
./configure --prefix=%{_prefix} --libdir=%{_libdir}/bufmgr

make %{?jobs:-j%jobs}

%install
%make_install


%post
if [ -f %{_libdir}/bufmgr/libtbm_default.so ]; then
    rm -rf %{_libdir}/bufmgr/libtbm_default.so
fi
ln -s libtbm_exynos4412.so %{_libdir}/bufmgr/libtbm_default.so

%postun -p /sbin/ldconfig

%files
%{_libdir}/bufmgr/libtbm_*.so*

