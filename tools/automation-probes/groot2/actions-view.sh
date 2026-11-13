# shellcheck shell=bash
# Sourced by shot.sh. Dismiss any dialog (recording it), capture the tree as
# drawn, zoomed out, and one zoomed-in detail; then save with Ctrl+S.
n=0
while [ $n -lt 12 ]; do
  w="$(xdotool search --onlyvisible --name '.' 2>/dev/null | while read -r x; do t="$(xdotool getwindowname "$x" 2>/dev/null)"; case "$t" in "Groot 2"*) ;; *) echo "$x"; break;; esac; done)"
  [ -n "$w" ] || break
  n=$((n+1))
  echo "dialog $n: $(xdotool getwindowname "$w") $(xdotool getwindowgeometry "$w" | tr '\n' ' ')"
  snap "dialog$n"
  xdotool key --window "$w" Return; xdotool key Return
  sleep 0.8
done
echo "dialogs dismissed: $n"
sleep 1.5
snap asdrawn
# zoom out over the canvas (wheel down x6), capture, then back in on the centre
xdotool mousemove 1000 520
for _ in 1 2 3 4 5 6; do xdotool click 5; sleep 0.15; done
sleep 1; snap zoomout
for _ in 1 2 3 4 5 6 7 8 9 10; do xdotool click 4; sleep 0.15; done
sleep 1; snap zoomin
if [ "${DO_SAVE:-0}" = 1 ]; then
  xdotool mousemove 1000 520 click 1; sleep 0.3
  xdotool key ctrl+s; sleep 2
  echo "windows after save:"; xdotool search --onlyvisible --name '.' 2>/dev/null | while read -r x; do echo "  $(xdotool getwindowname "$x" 2>/dev/null)"; done
  snap aftersave
fi
