# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(pipe-rw-close) begin
(pipe-rw-close) open pipe
(pipe-rw-close) end
pipe-rw-close: exit(0)
EOF
pass;
