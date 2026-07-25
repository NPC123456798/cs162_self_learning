# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(filesize-bad) begin
(filesize-bad) filesize of -1
(filesize-bad) filesize of unopened fd 5
(filesize-bad) filesize of invalid fd 128
(filesize-bad) end
filesize-bad: exit(0)
EOF
pass