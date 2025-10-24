# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(pipe-long) begin
(pipe-long) open pipe
(pipe-long) exec child
child-long: exit(0)
(pipe-long) end
pipe-long: exit(0)
EOF
pass;
