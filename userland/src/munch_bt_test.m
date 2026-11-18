#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>

@interface MunchScanner : NSObject <CBCentralManagerDelegate>
@property (nonatomic) unsigned reports;
@property (nonatomic) BOOL poweredOn;
@end

@implementation MunchScanner
- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
	printf("munch-bt-test: CoreBluetooth state %ld\n", (long)central.state);
	if (central.state == CBManagerStatePoweredOn) {
		self.poweredOn = YES;
		[central scanForPeripheralsWithServices:nil options:nil];
	}
	fflush(stdout);
}

- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *, id> *)advertisementData RSSI:(NSNumber *)RSSI
{
	(void)central;
	(void)peripheral;
	(void)advertisementData;
	(void)RSSI;
	self.reports++;
}
@end

int
main(int argc, char **argv)
{
	@autoreleasepool {
		long seconds = argc > 1 ? strtol(argv[1], NULL, 10) : 30;
		if (seconds < 1 || seconds > 180) {
			return 64;
		}
		MunchScanner *scanner = [MunchScanner new];
		CBCentralManager *central = [[CBCentralManager alloc] initWithDelegate:scanner queue:nil];
		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
		if (central.isScanning) {
			[central stopScan];
		}
		printf("munch-bt-test: PoweredOn=%d reports=%u\n", scanner.poweredOn, scanner.reports);
		return scanner.poweredOn && scanner.reports != 0 ? 0 : 1;
	}
}
