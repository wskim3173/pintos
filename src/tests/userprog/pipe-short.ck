# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(pipe-short) begin
(pipe-short) open pipe
(pipe-short) exec child
(child-short) received a msg
child-short: exit(0)
(pipe-short) end
pipe-short: exit(0)
EOF
pass;
