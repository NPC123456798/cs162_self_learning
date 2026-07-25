# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(filesize) begin
(filesize) create data.txt with size 100
(filesize) open data.txt
(filesize) filesize equals initial size 100
(filesize) end
filesize: exit(0)
EOF
pass;