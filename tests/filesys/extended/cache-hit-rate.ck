use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(cache-hit-rate) begin
(cache-hit-rate) create file
(cache-hit-rate) open for write
(cache-hit-rate) open for cold read
(cache-hit-rate) open for hot read
(cache-hit-rate) cache hit rate verified
(cache-hit-rate) cache hit rate test passed
(cache-hit-rate) end
cache-hit-rate: exit(0)
EOF
pass;