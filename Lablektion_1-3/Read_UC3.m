% Read_UC3.m
% Use this function in uc3 project
% void User_Uart_Put_Matlab(uint16_t *User_Arrary)
% 
% TERMINAT script with CTRL+C
% run "fclose(instrfind)" each time when this program is terminated
%
% If error comes check serial port number
% Copyright (c) 2014 Ping Wu. All rights reserved.

clear;
x=0;
a=0;
vect=0;
s=0;
b=0;
sta=0;
che=0;
status = 0;
%Fs = 12000000/256; 
Fs = 132000; %46875;  %89280; 
cal=0;
datasize=1024;

uc3port=serial('com5');    % Port "com?" may vary every time when the USB is plugged in
% go to Device Manager and check the port number for the USB-RS232 cable
set(uc3port, 'InputBufferSize', 256); %number of bytes in inout buffer
set(uc3port, 'FlowControl', 'none');
set(uc3port, 'BaudRate', 57600);
set(uc3port, 'Parity', 'none');
set(uc3port, 'DataBits', 8);
set(uc3port, 'StopBit', 1);
set(uc3port, 'TimeOut', 60);
% if (Status(uc3port)==-1)
%     fclose(instrfind);
%     fopen(uc3port);
% end

fopen(uc3port);
disp(get(uc3port,'Name'));
prop(1)=(get(uc3port,'BaudRate'));
prop(2)=(get(uc3port,'DataBits'));
prop(3)=(get(uc3port, 'StopBit'));
prop(4)=(get(uc3port, 'InputBufferSize'));
 
disp('Port Setup Done!!');
disp(prop);
fid=fopen('test1.txt','wt');
while(1)
    recdta = fread(uc3port,1,'uint8');
    if  recdta=='M';
        sta=1;
        che=0;
    end
    if  recdta=='T';
        sta=sta+1;
    end
    if  recdta=='L';
        sta=sta+1;
    end
    if  recdta=='A';
        sta=sta+1;
    end
    if  recdta=='B';
        sta=sta+1;
    end
    che=che+1;
     if  (sta==6&&che==sta);
        che=0;
        sta=0;
        disp('Data recieving, Data saving to test1.txt')
        x=0;
        clear vect;
        vect = 0;
        Nd = 16;      % the number of data that are read each time
        while (x <datasize/Nd)                       
            recdta = fread(uc3port,Nd*6,'uint8'); %6 = the length of each data!
            for i=1:length(recdta)
               fprintf('%c',recdta(i));
                fwrite(fid,recdta(i),'uint8');
                if recdta(i) ~= char(',')
                    a=a+1;
                    s = [s char(recdta(i))];
                else
                    [b,status]=str2num(s);
                    if a==5;
                        vect = [vect b-cal];
                    end
                    s=0;
                    a=0;
                end
               
            end
             t= linspace(0,length(vect),length(vect))/Fs;  % Sampling frequency 12MHz/256 = 46875
%             t = t*1e6;      % converted to us
             t = t*1e3;      % converted to ms
             plot(t,vect);
             grid on;
%             xlabel('time  : us')
             xlabel('time  : ms')
             drawnow;
            disp(' ');
            x=x+1;          
        end
          disp('  ');
          break;
    end
    
end

fclose(instrfind);