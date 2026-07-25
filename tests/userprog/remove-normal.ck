# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(remove-normal) begin
(remove-normal) create temp.txt
(remove-normal) remove temp.txt
(remove-normal) remove nonexistent temp.txt
(remove-normal) open removed temp.txt
(remove-normal) end
remove-normal: exit(0)
EOF
pass