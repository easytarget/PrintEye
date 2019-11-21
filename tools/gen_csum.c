// Generate checksums for a few commands as needed
// Owen, 20190908
// lifted from: https://duet3d.dozuki.com/Wiki/Gcode#Section_Checking

#include <stdio.h>

void csum(char cmd[56])
{
  int cs = 0;
  for(int i = 0; cmd[i] != '*' && cmd[i] != NULL; i++)
   cs = cs ^ cmd[i];
  cs &= 0xff;  // Defensive programming...
  printf("%s*%u\n",cmd,cs);
}

void main()
{
  csum("M408 S0");
  csum("M24");
  csum("M25");
  csum("M112");
  csum("M118 P2 S\"//action:pause\"");
  csum("M118 P2 S\"//action:resume\"");
}

