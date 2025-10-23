# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(pipe-bad-write) begin
(pipe-bad-write) open pipe
(pipe-bad-write) write prohibited
(pipe-bad-write) end
pipe-bad-write: exit(0)
EOF
pass;
