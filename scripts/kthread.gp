# 設定輸出
set terminal pngcairo enhanced font 'Arial,12' size 1000,600
set output 'kthread_run_cost.png'

# 標題和軸標籤
set title "Kernel Thread Creation Time"
set xlabel "Request Count"
set ylabel "Time (us)"

# 強制 Y 軸從 0 開始
set yrange [0:*]

# 設定 Y 軸刻度間隔為 10 (us)
set ytics 10

# 網格（設定主要刻度與次要刻度網格）
set grid ytics

# 繪圖（十字點，不要線）
plot 'eBPF/kthread_run_cost.txt' skip 1 using 0:($1/1000.0) with points \
    lc rgb '#0060ad' pt 2 ps 1.0 title 'Duration'