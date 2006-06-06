# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Library General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

import sqlite
import _sqlitecache

class RepodataParserSqlite:
    def __init__(self, storedir, repoid, callback=None):
		self.callback = callback

    def open_database(self, filename):
        if not filename:
            return None
        return sqlite.connect(filename)

    def getPrimary(self, location, checksum):
        """Load primary.xml.gz from an sqlite cache and update it 
           if required"""
        return self.open_database(_sqlitecache.update_primary(location,
															  checksum,
															  self.callback))

    def getFilelists(self, location, checksum):
        """Load filelist.xml.gz from an sqlite cache and update it if 
           required"""
        return self.open_database(_sqlitecache.update_filelist(location,
															   checksum,
															   self.callback))

    def getOtherdata(self, location, checksum):
        """Load other.xml.gz from an sqlite cache and update it if required"""
        return self.open_database(_sqlitecache.update_other(location,
															checksum,
															self.callback))
    
