# How to do version updates:
# bump one or more of the 3 defines below
# osc service localrun download_files
#	to download the tarballs.
# rm the old tarballs and signature
# osc addremove
# osc ci
# check buildresult after a while

%define _lto_cflags %{nil}
%define realver     7.0
%define stablever	7.0.0

%global flavor @BUILD_FLAVOR@%nil

# defaults ..
%define pkg 		wine-stable
%define compat_package	winehq-stable
%define _prefix 	/opt/wine-stable
%define ver		%stablever

%define lib_major       1
%define lib_name        lib%{pkg}1
%define lib_name_devel  lib%{pkg}-devel
%undefine _hardened_build

Name:       %pkg
Version:    %ver
Release:    0
Epoch:      1
Summary:    WINE Is Not An Emulator - runs MS Windows programs
License:    LGPLv2+
Group:      Emulators
URL:        https://www.winehq.org/

%ifarch x86_64
%define wine    %{name}64
%define mark64  ()(64bit)
%else
%define wine    %{name}
%define mark64  %{nil}
%endif

Source2:	https://dl.winehq.org/wine/source/6.0/wine-%{realver}.tar.xz
Source3:	https://dl.winehq.org/wine/source/6.0/wine-%{realver}.tar.xz.sign
#Patch0:     4f04994ef47b5077e13c1b770ed0f818f59adcd5.patch
# Alexandres key
Source99:	wine.keyring

# These are all the Fedora only packages, which are not present on Centos, RHEL or Scientific Linux
%if 0%{?fedora_version}
BuildRequires:  mingw32-gcc
BuildRequires:  mingw64-gcc
BuildRequires:  SDL2-devel
BuildRequires:  openal-soft-devel
BuildRequires:  opencl-headers
BuildRequires:  ocl-icd-devel
BuildRequires:	libFAudio-devel
BuildRequires:  libvkd3d-devel
BuildRequires:  libvkd3d-shader-devel
%endif

# Fedora and SL 7.0 packages
%if 0%{?fedora_version} || 0%{?scientificlinux_version} >= 700 || 0%{?centos_version} >= 700
BuildRequires:  icoutils
BuildRequires:  vulkan-devel
BuildRequires:  lcms2-devel
BuildRequires:  gstreamer1-devel
BuildRequires:  gstreamer1-plugins-base-devel
BuildRequires:  libmpg123-devel
%endif

# On all Redhat based versions except RHEL...
%if 0%{?fedora_version} || 0%{?scientificlinux_version} || 0%{?centos_version}
BuildRequires:  fontforge
BuildRequires:  fontpackages-devel
BuildRequires:  gsm-devel
BuildRequires:  libjpeg-turbo-devel
BuildRequires:  libudev-devel
BuildRequires:  libv4l-devel
BuildRequires:  pulseaudio-libs-devel
BuildRequires:  lzma
BuildRequires:  audiofile-devel
BuildRequires:  giflib-devel
BuildRequires:  ImageMagick-devel
BuildRequires:  libpcap-devel
BuildRequires:  libXxf86dga-devel
BuildRequires:  mesa-libOSMesa-devel
BuildRequires:  libgphoto2-devel
BuildRequires:  libusb-devel
%endif

%if 0%{?fedora_version} < 31
BuildRequires:  isdn4k-utils-devel
%endif

BuildRequires:  alsa-lib-devel
BuildRequires:  autoconf
BuildRequires:  bison
BuildRequires:  coreutils
BuildRequires:  cups-devel
BuildRequires:  dbus-devel
BuildRequires:  desktop-file-utils
BuildRequires:  flex
BuildRequires:  fontconfig-devel
BuildRequires:  freetype-devel
BuildRequires:  freeglut-devel
BuildRequires:  gawk
BuildRequires:  xz
BuildRequires:  gettext-devel
BuildRequires:  gnutls-devel
BuildRequires:  krb5-devel
BuildRequires:  libattr-devel
BuildRequires:  libieee1284-devel
BuildRequires:  libpng-devel
BuildRequires:  librsvg2
BuildRequires:  librsvg2-devel
BuildRequires:  libstdc++-devel
BuildRequires:  libtiff-devel
BuildRequires:  libX11-devel
BuildRequires:  libXcomposite-devel
BuildRequires:  libXcursor-devel
BuildRequires:  libXext-devel
BuildRequires:  libXi-devel
BuildRequires:  libXinerama-devel
BuildRequires:  libxml2-devel
BuildRequires:  libXmu-devel
BuildRequires:  libXrandr-devel
BuildRequires:  libXrender-devel
BuildRequires:  libxslt-devel
BuildRequires:  libXxf86vm-devel
BuildRequires:  mesa-libGL-devel
BuildRequires:  mesa-libGLU-devel
BuildRequires:  ncurses-devel
BuildRequires:  openldap-devel
BuildRequires:  sane-backends-devel
BuildRequires:  unixODBC-devel
BuildRequires:  unzip
BuildRequires:  util-linux
BuildRequires:  zlib-devel

%ifarch x86_64
%package -n %{wine}
%endif
Summary:    WINE Is Not An Emulator - runs MS Windows programs
Group:      Emulators
%ifarch x86_64
Conflicts:  %{name}
%else
Conflicts:  %{name}64
%endif
Requires:   %{name}-common = %{epoch}:%{version}-%{release}
Provides:   %{lib_name} = %{epoch}:%{version}-%{release}
Obsoletes:  %{lib_name} <= %{epoch}:%{version}-%{release}
Provides:   %{name}-bin = %{epoch}:%{version}-%{release}

%ifarch %{ix86}
%package -n %{name}-common
Summary:    WINE Is Not An Emulator - runs MS Windows programs (32-bit common files)
Group:      Emulators
Requires:   %{name}-bin = %{epoch}:%{version}-%{release}
%endif

%define dlopenreq() %(F=/usr/%{_lib}/lib%{1}.so;[ -e $F ] && (file $F|grep -q ASCII && grep -o 'lib[^ ]*' $F|sed -e "s/\$/%{mark64}/"||objdump -p $F | grep SONAME | awk '{ print $2 "%{mark64}" }') || echo "wine-missing-buildrequires-on-%{1}")
Requires:   %dlopenreq asound
Requires:   %dlopenreq attr
Requires:   %dlopenreq cups
Requires:   %dlopenreq dbus-1
Requires:   %dlopenreq fontconfig
Requires:   %dlopenreq freetype
Requires:   %dlopenreq gnutls
Requires:   %dlopenreq gsm
Requires:   %dlopenreq jpeg
Requires:   %dlopenreq krb5
Requires:   %dlopenreq ncurses
Requires:   %dlopenreq odbc
Requires:   %dlopenreq OSMesa
Requires:   %dlopenreq png
Requires:   %dlopenreq sane
Requires:   %dlopenreq SDL2
Requires:   %dlopenreq tiff
Requires:   %dlopenreq v4l1
Requires:   %dlopenreq vulkan
Requires:   %dlopenreq Xcomposite
Requires:   %dlopenreq Xcursor
Requires:   %dlopenreq Xi
Requires:   %dlopenreq Xinerama
Requires:   %dlopenreq Xrandr
Requires:   %dlopenreq Xrender
Requires:   %dlopenreq xslt
Requires:   %dlopenreq Xxf86vm
%if 0%{?fedora_version}
Suggests:   sane-frontends
%endif
Requires(post): desktop-file-utils
Requires(postun): desktop-file-utils

%define desc Wine is a program which allows running Microsoft Windows programs \
(including DOS, Windows 3.x and Win32 executables) on Unix. It \
consists of a program loader which loads and executes a Microsoft \
Windows binary, and a library (called Winelib) that implements Windows \
API calls using their Unix or X11 equivalents.  The library may also \
be used for porting Win32 code into native Unix executables.

%description
%desc

%ifarch x86_64
%description -n %{wine}
%desc
%else
%description -n %{name}-common
Wine is a program which allows running Microsoft Windows programs
(including DOS, Windows 3.x and Win32 executables) on Unix.

This package contains the files needed to support 32-bit Windows
programs, and is used by both %{name} and %{name}64.
%endif

%package -n %{wine}-devel
Summary:    Static libraries and headers for %{name} (64-bit)
Group:      Development/C
Requires:   %{wine} = %{epoch}:%{version}-%{release}
%ifarch x86_64
Conflicts:  %{name}-devel
%else
Conflicts:  %{name}64-devel
%endif
Provides:   %{lib_name_devel} = %{epoch}:%{version}-%{release}
Obsoletes:  %{lib_name_devel} <= %{epoch}:%{version}-%{release}
%description -n %{wine}-devel
Wine is a program which allows running Microsoft Windows programs
(including DOS, Windows 3.x and Win32 executables) on Unix.

This package contains the libraries and header files needed to
develop programs which make use of Wine.

%package -n %compat_package
Summary:    WINE Is Not An Emulator - runs MS Windows programs
Group:      Emulators
Requires:   %{wine} = %{epoch}:%{version}-%{release}
Conflicts:  wine wine64 wine-core wine-common wine-desktop wine-devel

%description -n %compat_package
Wine is a program which allows running Microsoft Windows programs
(including DOS, Windows 3.x and Win32 executables) on Unix.

This compatibility package allows to use %{wine} system-wide as
the default Wine version.

%prep
# unpack tarball SOURCE2
# %setup -n wine-%version -q -T -b2
%setup -n wine-%realver -q -T -b2
#%if 0%{?fedora_version} >= 34
#%patch0 -p1
#%endif

%build
%ifarch x86_64
export CFLAGS="$(echo "%{optflags}" | sed -e 's/-O2//' -e 's/-Wp,-D_FORTIFY_SOURCE=2//' -e 's/-fcf-protection//' -e 's/-fstack-protector-strong//' -e 's/-fstack-clash-protection//') -O2"
%else
export CFLAGS="$(echo "%{optflags}" | sed -e 's/-Wp,-D_FORTIFY_SOURCE=2//' -e 's/-fcf-protection//' -e 's/-fstack-protector-strong//' -e 's/-fstack-clash-protection//')"
%endif
%configure \
%if 0%{?fedora_version} || 0%{?scientificlinux_version} >= 700 || 0%{?centos_version} >= 700
    --with-gstreamer \
%endif
    --disable-tests \
%ifarch x86_64
    --enable-win64 \
%endif
    --with-x

make -j4

%install
%makeinstall LDCONFIG=/bin/true

# Compat symlinks for bindir
mkdir -p "%{buildroot}/usr/bin"
for _file in $(ls "%{buildroot}/%{_bindir}"); do \
    ln -s "%{_bindir}/$_file" "%{buildroot}/usr/bin/$_file"; \
done
%ifarch x86_64
for _file in wine wine-preloader; do \
    ln -s "%{_prefix}/bin/$_file" "%{buildroot}/usr/bin/$_file"; \
done
%endif

# Compat symlinks for desktop file
mkdir -p "%{buildroot}/usr/share/applications"
for _file in $(ls "%{buildroot}/%{_datadir}/applications"); do \
    ln -s "%{_datadir}/applications/$_file" "%{buildroot}/usr/share/applications/$_file"; \
done

# Compat manpages
%if  0%{?fedora_version} || 0%{?scientificlinux_version} || 0%{?centos_version} >= 700 || 0%{?rhel_version} >= 700
for _dir in man1 de.UTF-8/man1 fr.UTF-8/man1 pl.UTF-8/man1; do \
	if [ -d "%{buildroot}/%{_mandir}/$_dir" ]; then \
        mkdir -p "$(dirname "%{buildroot}/usr/share/man/$_dir")"; \
		cp -pr "%{buildroot}/%{_mandir}/$_dir" "%{buildroot}/usr/share/man/$_dir"; \
	else \
		mkdir -p "%{buildroot}/usr/share/man/$_dir"; \
	fi; \
done
%else
for _dir in man1 de.UTF-8/man1 fr.UTF-8/man1 pl.UTF-8/man1; do \
        mkdir -p "%{buildroot}/usr/share/man/$_dir"; \
done
%endif

%ifarch x86_64
install -p -m 0644 loader/wine.man          "%{buildroot}/usr/share/man/man1/wine.1"
install -p -m 0644 loader/wine.de.UTF-8.man "%{buildroot}/usr/share/man/de.UTF-8/man1/wine.1"
install -p -m 0644 loader/wine.fr.UTF-8.man "%{buildroot}/usr/share/man/fr.UTF-8/man1/wine.1"
install -p -m 0644 loader/wine.pl.UTF-8.man "%{buildroot}/usr/share/man/pl.UTF-8/man1/wine.1"
%endif

%files -n %{wine}
%doc ANNOUNCE AUTHORS README
%ifarch x86_64
%{_bindir}/wine64
%{_bindir}/wine64-preloader
%endif
%{_bindir}/function_grep.pl
%{_bindir}/msiexec
%{_bindir}/msidb
%{_bindir}/notepad
%{_bindir}/regedit
%{_bindir}/regsvr32
%{_bindir}/widl
%{_bindir}/wineboot
%{_bindir}/winebuild
%{_bindir}/winecfg
%{_bindir}/wineconsole*
%{_bindir}/winecpp
%{_bindir}/winedbg
%{_bindir}/winedump
%{_bindir}/winefile
%{_bindir}/wineg++
%{_bindir}/winegcc
%{_bindir}/winemaker
%{_bindir}/winemine
%{_bindir}/winepath
%{_bindir}/wineserver
%{_bindir}/wmc
%{_bindir}/wrc
%lang(de) %{_mandir}/de.UTF-8/man?/winemaker.?*
%lang(de) %{_mandir}/de.UTF-8/man?/wineserver.?*
%lang(fr) %{_mandir}/fr.UTF-8/man?/winemaker.?*
%lang(fr) %{_mandir}/fr.UTF-8/man?/wineserver.?*
%{_mandir}/man?/widl.1*
%{_mandir}/man?/winebuild.1*
%{_mandir}/man?/winecpp.1*
%{_mandir}/man?/winedbg.1*
%{_mandir}/man?/winedump.1*
%{_mandir}/man?/wineg++.1*
%{_mandir}/man?/winegcc.1*
%{_mandir}/man?/winemaker.1*
%{_mandir}/man?/wmc.1*
%{_mandir}/man?/wrc.1*
%{_mandir}/man?/msiexec.?*
%{_mandir}/man?/notepad.?*
%{_mandir}/man?/regedit.?*
%{_mandir}/man?/regsvr32.?*
%{_mandir}/man?/wineboot.?*
%{_mandir}/man?/winecfg.?*
%{_mandir}/man?/wineconsole.?*
%{_mandir}/man?/winefile.?*
%{_mandir}/man?/winemine.?*
%{_mandir}/man?/winepath.?*
%{_mandir}/man?/wineserver.?*
%dir %{_datadir}/wine
%{_datadir}/wine/wine.inf
%{_datadir}/wine/nls/*.nls
%{_datadir}/applications/*.desktop
%dir %{_datadir}/wine/fonts
%{_datadir}/wine/fonts/*

%ifarch %{ix86}
%files -n %{name}-common
%{_bindir}/wine
%{_bindir}/wine-preloader
%{_mandir}/man?/wine.?*
%lang(de) %{_mandir}/de.UTF-8/man?/wine.?*
%lang(fr) %{_mandir}/fr.UTF-8/man?/wine.?*
%lang(pl) %{_mandir}/pl.UTF-8/man?/wine.?*
%endif

%ifarch %{ix86}
%{_libdir}/wine/i386-unix/*.*
%{_libdir}/wine/i386-windows/*.*
%endif
%ifarch x86_64
%{_libdir}/wine/x86_64-unix/*.*
%{_libdir}/wine/x86_64-windows/*.*
%endif

%files -n %{wine}-devel
%{_includedir}/*

%files -n %compat_package
/usr/bin/*
/usr/share/applications/*.desktop
/usr/share/man/man?/*
%lang(de) /usr/share/man/de.UTF-8/man?/*
%lang(fr) /usr/share/man/fr.UTF-8/man?/*
%lang(pl) /usr/share/man/pl.UTF-8/man?/*
