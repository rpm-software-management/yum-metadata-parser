%{!?python_sitelib_platform: %define python_sitelib_platform %(%{__python} -c "from distutils.sysconfig import get_python_lib; print get_python_lib(1)")}

Summary: A fast metadata parser for yum
Name: yum-metadata-parser
Version: 1.0.3
Release: 1%{?dist}
Source0: %{name}-%{version}.tar.gz
License: GPL
Group: Development/Libraries
URL: http://devel.linux.duke.edu/cgi-bin/viewcvs.cgi/yum-metadata-parser/
Requires: yum >= 2.6.2
BuildRequires: python-devel
BuildRequires: glib2-devel
BuildRequires: libxml2-devel
BuildRequires: sqlite-devel
BuildRequires: pkgconfig
BuildRoot:  %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

%description
Fast metadata parser for yum implemented in C.

%prep
%setup

%build
%{__python} setup.py build

%install
%{__python} setup.py install -O1 --root=%{buildroot}

%clean
%{__rm} -rf %{buildroot}

%files
%defattr(-,root,root)
%doc README AUTHORS ChangeLog
%{python_sitelib_platform}/_sqlitecache.so
%{python_sitelib_platform}/sqlitecachec.py
%{python_sitelib_platform}/sqlitecachec.pyc
%{python_sitelib_platform}/sqlitecachec.pyo

%changelog
* Sun Jan  7 2007 Seth Vidal <skvidal at linux.duke.edu>
- 1.0.3

* Wed Jul 12 2006 Seth Vidal <skvidal at linux.duke.edu>
- 1.0.2

* Mon Jun 19 2006 Seth Vidal <skvidal at linux.duke.edu>
- 1.0.1

* Mon Jun 05 2006 Tambet Ingo <tambet@ximian.com> - 1.0-3
- Require yum >= 2.6.2

* Sat Jun 04 2006 Terje Rosten <terje.rosten@pvv.org> - 1.0-2
- add buildrequires
- doc files
- url

* Fri Jun 02 2006 Terje Rosten <terje.rosten@pvv.org> - 1.0-0.1
- initial package

