#!/bin/bash -e
# $File: gen_exe_status.sh
# $Author: Jiakai <jia.kai66@gmail.com>
# $Date: Sat Sep 18 11:47:55 2010 +0800
#
# This file is part of orzoj
# 
# Copyright (C) <2010>  Jiakai <jia.kai66@gmail.com>
# 
# Orzoj is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
# 
# Orzoj is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with orzoj.  If not, see <http://www.gnu.org/licenses/>.
#

FILE='exe_status.h'

cat > $FILE << _EOF_
/* this file is automatically generated by `basename $0` */
#ifndef _HEADER_EXE_STATUS_
#define _HEADER_EXE_STATUS_
typedef unsigned int Exests_t;
_EOF_

id=0

for i in $(cat ../../../structures.py | grep -o '^EXESTS[A-Z_]*')
do
	echo "static const Exests_t $i = $id;" >> $FILE
	let id=$id+1
done


echo '#endif' >> $FILE

