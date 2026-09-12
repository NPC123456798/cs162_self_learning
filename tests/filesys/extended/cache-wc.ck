use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(cache-wc) begin
(cache-wc) create file
(cache-wc) open for write
(cache-wc) write coalescing verified
(cache-wc) cache write coalesce test passed
(cache-wc) end
cache-wc: exit(0)
EOF
pass;