import os
try:
	from setuptools import setup
except ImportError:
	from distutils.core import setup
from distutils.core import Extension

pc = os.popen("pkg-config --cflags-only-I glib-2.0 libxml-2.0 sqlite3", "r")
includes = list(map(lambda x:x[2:], pc.readline().split()))
pc.close()

pc = os.popen("pkg-config --libs-only-l glib-2.0 libxml-2.0 sqlite3", "r")
libs = list(map(lambda x:x[2:], pc.readline().split()))
pc.close()

pc = os.popen("pkg-config --libs-only-L glib-2.0 libxml-2.0 sqlite3", "r")
libdirs = list(map(lambda x:x[2:], pc.readline().split()))
pc.close()

module = Extension('_sqlitecache',
                   include_dirs = includes,
                   libraries = libs,
                   library_dirs = libdirs,
                   sources = ['package.c',
                              'xml-parser.c',
                              'db.c',
                              'sqlitecache.c'])

setup (name = 'yum-metadata-parser',
       version = '1.1.4',
       description = 'A fast YUM meta-data parser',
       py_modules = ['sqlitecachec'],
       ext_modules = [module],
       use_scm_version=True)
