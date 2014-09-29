cat >stats.html <<EOF
<html>
<body>
EOF

find | grep log | grep -v parsed | while read line; do
    prefix="${line%.log}"
    board="${line%/*}"
    board="${board#./}"
    sh perfparse "$line" 1>&2
    R -f plot.R --args "$prefix" 1>&2
    echo "<h1>$board - `sed -e 's/^# //' "$prefix.title"`</h1>"
    echo "<small>$line</small><br>"
    awk -F';' '{ printf "%s: %.1f%%; ", $2, $1 }' "$prefix.total"
    echo "<table><tr>"
    echo "<td><img src='$prefix.proc.png'></td>"
    echo "<td><img src='$prefix.total.png'></td>"
    echo "</tr></table>"
done >> stats.html

cat >>stats.html <<EOF
</body>
</html>
EOF
