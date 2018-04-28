// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
#include <memory>

class DeliveryNotificationManager;

class TransmissionData
{
public:
	virtual void HandleDeliveryFailure( DeliveryNotificationManager* inDeliveryNotificationManager ) const = 0;
	virtual void HandleDeliverySuccess( DeliveryNotificationManager* inDeliveryNotificationManager ) const = 0;
};
typedef std::shared_ptr< TransmissionData > TransmissionDataPtr;