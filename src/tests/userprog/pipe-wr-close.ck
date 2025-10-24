# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(pipe-wr-close) begin
(pipe-wr-close) open pipe
(pipe-wr-close) end
pipe-wr-close: exit(0)
EOF
pass;
