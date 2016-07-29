Name:           libtbm-android
Version:        0.9.0
Release:        1
License:        MIT
Summary:        Tizen Buffer Manager - android backend
Group:          System/Libraries
Source0:        %{name}-%{version}.tar.gz

%description
Descriptionion: Tizen Buffer manager backend module for android.

%prep
%setup -q -c %{name}-%{version}

%build
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
./autogen.sh --build=x86_64-unknown-linux-gnu \
	     --host=arm-linux-androideabi
	     
make %{?_smp_mflags}

%install
rm -rf %{buildroot}

%make_install
cd %{buildroot}/usr/local/lib/bufmgr/ && ln -s libtbm-android.so libtbm-default.so

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root,-)
/usr/local/*
