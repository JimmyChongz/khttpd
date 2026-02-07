# 設定輸出
set terminal pngcairo enhanced font 'Arial,12' size 1000,600
set output 'kthread_run_cost.png'

# 標題和軸標籤
set title "Kernel Thread Creation Time"
set xlabel "Request Count"
set ylabel "Time (us)"

# 網格
set grid

# 繪圖（十字點，不要線）
plot 'eBPF/kthread_run_cost.txt' skip 1 using 0:($1/1000.0) with points \
    lc rgb '#0060ad' pt 2 ps 1.0 title 'Duration'