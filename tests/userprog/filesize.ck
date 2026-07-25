# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(filesize) begin
(filesize) create data.txt
(filesize) open data.txt
(filesize) filesize is 100 at creation
(filesize) end
filesize: exit(0)
EOF
pass