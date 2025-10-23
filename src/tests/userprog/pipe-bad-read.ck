# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(pipe-bad-read) begin
(pipe-bad-read) open pipe
(pipe-bad-read) read prohibited
(pipe-bad-read) end
pipe-bad-read: exit(0)
EOF
pass;
