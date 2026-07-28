/* app/leapyear.c */
#include<hax.h>
#include<libc/stdlib.h> /* atoi */
/* #include<unistd.h> */

HAX_APP("leapyear","闰年判定",HAX_KIND_BOTH);
char line[32];

int main(int argc,char **argv){
    (void)argc;(void)argv;
    hax_println("  _       _____      _      ____   __   __  _____      _      ____  " );
    hax_println(" | |     | ____|    / \\    |  _ \\  \\ \\ / / | ____|    / \\    |  _ \\  ");
    hax_println(" | |     |  _|     / _ \\   | |_) |  \\ V /  |  _|     / _ \\   | |_) | ");
    hax_println(" | |___  | |___   / ___ \\  |  __/    | |   | |___   / ___ \\  |  _ <  ");
    hax_println(" |_____| |_____| /_/   \\_\\ |_|       |_|   |_____| /_/   \\_\\ |_| \\_\\ ");
    while(true){
        hax_println("请输入要判定的年份(输入 q 退出)> ");
        if (hax_input(line,sizeof(line))<0||line[0]=='q'){
            /* hax_println("应用已退出。输入 'run leapyear' 或点击应用图标再次打开\n"); */
            /* return 0; */
            break;
        }
        int year=atoi(line);
        // if(year>10000||year<-10000||year==0)
        if(year>10000||year<=0){
            hax_println("输入的年份须为 1~10000 之间的整数(不能为0或非数字、负号-或q以外的字符)。\n");
            continue;
            /* hax_println("应用已退出。输入 'run leapyear' 或点击应用图标再次打开\n"); */
        }
        else if(year%100==0){
            if(year%400==0){
                hax_printf("%d年是闰年\n",year);
                continue;
            }
            else{
                hax_printf("%d年不是闰年\n",year);
                continue;
            }
        }
        else{
            if(year%4==0){
                hax_printf("%d年是闰年\n",year);
                continue;
            }
            else{
                hax_printf("%d年不是闰年\n",year);
                continue;
            }
        }
    }
    hax_println("应用已退出。输入 'run leapyear' 或点击应用图标再次打开\n");
    /* sleep(500); */
    return 0;
}
